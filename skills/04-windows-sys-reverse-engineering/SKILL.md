---
name: windows-sys-reverse-engineering
description: Windows 커널 드라이버(.sys, PE x86)를 objdump로 디스어셈블해 USB 명령 패킷 생성 함수, GPIO 전원/리셋 순서, I2C 레지스터 초기화 테이블, 튜너 IF 같은 보드 고유 설정을 찾아낸다. 장치 프로토콜은 알지만 보드 설정값을 모를 때 사용.
---

# Windows .sys 드라이버에서 보드 설정 찾기

## 준비
```sh
objdump -d -M intel skyhdmc.sys > sys.asm
objdump -h skyhdmc.sys            # 섹션 VA ↔ 파일 오프셋 (이 파일은 file = VA - 0x10000)
```
x86 32비트 드라이버(`x86/`)가 amd64보다 읽기 쉽습니다(stdcall/thiscall, 인자가 스택에 push됨).

## 1. 명령 패킷을 만드는 함수 찾기
펌웨어 명령 바이트를 즉시값으로 넣는 곳을 찾습니다.
```sh
grep -nE "mov +BYTE PTR \[[^]]+\],0x(8|9|e|36|37)$" sys.asm
```
- `mov [ebp+0x8],0x0e` → GPIO 쓰기 함수 **SetGpioBit(port, bit, value)** (0x1b9a8)
- `mov [ebp-0x108],0x9` → I2C 읽기, `0x8` → I2C 쓰기. `shr cl,1`이 보이면 8-bit 주소를 받아 7-bit로 바꿔 보냄
- `mov [ebp-0x4],0x36` + `push 4` → 스트림 시작 `[36 03 00 x]`, `0x37` → 정지

## 2. 호출 지점에서 인자 읽기
stdcall은 인자를 **뒤에서부터** push합니다. 마지막 push가 첫 번째 인자입니다.
```sh
grep -n "call   0x1b9a8" sys.asm | while IFS=: read n r; do sed -n "$((n-7)),${n}p" sys.asm; done
```
```
push 0x1 ; push 0x3 ; push edi(=0) ; call SetGpioBit   →  SetGpioBit(port=0, bit=3, value=1)
```
이렇게 전원 함수 두 개와 리셋 함수를 복원했습니다.
- 튜너 전원: PD6=1, (지연 1 s), PD1=1
- 복조기 리셋: PD7 0 → 10 ms → 1
- XC5000 리셋 콜백(펌웨어 시퀀스 중 RESET): PD1 0 → 10 ms → 1
- 지연 함수 인자: `push 0x186a0`(100000 µs), `push 0xa; pop eax`(10 ms). 단위는 호출 함수 안의 `*-10000`(100 ns 단위)로 판단

## 3. I2C 주소로 레지스터 테이블 찾기
```sh
grep -nE "push   0xb2$|mov    esi,0xb2|push   0xc2$" sys.asm    # 복조기 0xB2, 튜너 0xC2 (8-bit)
```
`mov edi,<VA>` + `push 3` + `call [eax+4]` + `add edi,3` + `cmp ebx,0x3c` 루프 = **3바이트 레코드
`[reg_hi, reg_lo, val]` 20개를 쓰는 테이블**입니다. VA를 파일 오프셋으로 바꿔 덤프합니다.
```python
fo = va - 0x10000
print(' '.join(f"{d[k]:02x}{d[k+1]:02x}={d[k+2]:02x}" for k in range(0, n, 3)))
```
결과(VSB): `0000=03 000d=02 000e=02 0012=32 0013=c4 0112=17 … 0126=79/f9 … 0314=e1`

## 4. 테이블을 커널 드라이버와 대조
레지스터 집합이 커널 `lgdt3305.c`의 `lgdt3304_init_data`와 거의 같았으므로 **LGDT3304**로 확정했습니다
(INF DemodType=2와도 맞음). 0x0126=0x79/0xF9은 커널 `spectral_inversion()`의 값과 같고,
QAM 테이블의 0x0504=0xFF/0xFD도 마찬가지입니다.

## 5. 튜너 설정
- XC5000 레지스터 쓰기 함수(`[reg_hi reg_lo val_hi val_lo]`, 주소 0xC2)를 찾고 `push 0x5`(XREG_IF_OUT) 호출 지점을 찾습니다.
  계산식 `freq * 0x400 / 0xf4240`과 호출 인자 `push 0x5b8d80` → **IF = 6,000,000 Hz**
- 모드 레지스터 1/2에 쓰는 전역 워드(`ds:0x2730e`, `ds:0x2730c`) = 0x8002 / 0x00C0 (DTV6)
- 튜너 펌웨어 시퀀스 로더: `test eax,0x8000` + `and eax,0x7fff`(WAIT), `len==0`이면 리셋 함수 호출

## 6. 장치 기본값 읽기
Windows가 **쓰지 않는** 레지스터는 칩 기본값을 쓴다는 뜻입니다. 장치에서 직접 읽어 확인합니다.
- TP_CTRL_1(0x050e) 기본값 0x1b → 병렬 TS, 클럭 하강 에지, gated clock, valid high
- VSB NCO(0x0106~0x0109): Windows는 쓰지 않음 → 커널이 넣는 고정값을 브리지에서 0으로 되돌려야 했음(07)

## 주의
- 같은 테이블이 두 벌(옛 경로/새 경로) 있을 수 있습니다. 실제 I2C 주소(0xB2)와 함께 쓰이는 쪽을 고릅니다.
- 오디오 처리 등 관련 없는 `push 0x108` 같은 값에 속지 않도록, 호출 대상 함수가 무엇을 하는지 먼저 확인합니다.
