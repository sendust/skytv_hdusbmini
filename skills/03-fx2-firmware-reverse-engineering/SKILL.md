---
name: fx2-firmware-reverse-engineering
description: Cypress FX2(CY7C68013) 8051 펌웨어를 Windows 드라이버에서 추출하고 d52로 디스어셈블해 호스트 명령 프로토콜(EP1 명령 테이블, I2C/GPIO/스트림 명령, 응답 형식)을 알아낸다. FX2 기반 USB 장치의 리눅스 드라이버를 만들 때 사용.
---

# FX2 펌웨어 추출과 명령 프로토콜 분석

## FX2 기초 (필요한 만큼)
| 항목 | 내용 |
|---|---|
| 펌웨어 적재 | vendor request `0xA0`: `CPUCS(0xE600)=1`(8051 정지) → 0x0000부터 RAM 쓰기 → `CPUCS=0`(실행) |
| 재연결 | 펌웨어가 시작하면 renumeration으로 새 디스크립터로 다시 연결 (이 장치는 같은 VID/PID, iManufacturer="SKYDIGITAL") |
| 주요 레지스터 | `E6B8~E6BF` SETUPDAT, `E678/E679` I2CS/I2DAT, `E780` EP1OUTBUF, `E7C0` EP1INBUF, `E68D` EP1OUTBC, `E68F` EP1INBC, `E601` IFCONFIG, `E604` FIFORESET, `E618~` EPxFIFOCFG |
| GPIO | SFR `P0(0x80)`=IOA / `0xB2`=OEA, `P3(0xB0)`=IOD / `0xB5`=OED |
| EP1 버퍼 | 64바이트 (디스크립터가 512라고 해도 실제 버퍼는 64) |

## 1. 펌웨어 위치 찾기와 추출
```python
b = open('skyhdmc.sys','rb').read()
for s in range(0, len(b), 1024):                 # FX2 레지스터 접근 밀도
    n = b[s:s+1024].count(b'\x90\xe6')           # mov dptr,#E6xx
    if n > 3: print(hex(s), n)
```
밀도가 높은 구간 바로 앞에서 `02 xx xx`(LJMP reset vector)와 그 뒤의 인터럽트 벡터(`02 xx xx`가 8바이트 간격,
`AA` 패딩)를 찾습니다. 이 장치는 파일 오프셋 **0x1ee58**부터 16 KB가 펌웨어였습니다.
```python
open('fw.bin','wb').write(b[0x1ee58:0x1ee58+0x4000])
```

## 2. 디스어셈블
```sh
sudo apt-get install -y d52
d52 -b -d fw.bin          # fw.d52 생성, -d: 각 줄에 주소/바이트 주석 → "; 0233" 으로 grep 가능
grep -n "Xe6b9\|Xe780\|Xe678" fw.d52     # bRequest, EP1OUTBUF, I2C 접근 위치
```
- **vendor request 처리**: `E6B8` bit6(vendor) 분기 → 이 펌웨어는 `setb c; ret`(stall)뿐이었음.
  즉 제어는 vendor request가 아니라 **EP1 bulk 명령**으로 합니다.
- **EP1 명령 분기 찾기**: `mov dptr,#E780; movx a,@dptr; lcall X0946` 패턴.
  X0946은 Keil C51의 `switch` 헬퍼로, 호출 바로 뒤에 `[addrHi addrLo value]` 3바이트 항목이 이어지고
  `00 00` + 기본 주소로 끝납니다.
  ```
  01 97 08 | 02 33 09 | 00 d8 0b | 01 ca 0c | 02 69 0d | 02 93 0e | … | 00 00 00 eb
  ```
  → cmd 0x08 → 0x0197, 0x09 → 0x0233, 0x0d → 0x0269, 0x0e → 0x0293 …
- d52가 데이터로 오인한 구간은 라벨이 없습니다. `-d` 주석의 주소(`; 0233 `)로 grep해서 읽습니다.

## 3. 핸들러 해석 요령
각 핸들러에서 **어떤 버퍼 바이트가 어떤 레지스터/변수로 가는지**를 한 줄씩 추적합니다.

예) cmd 0x09 (0x0233):
```
mov dptr,#E783 → r7        ; buf[3] = I2C 주소
mov dptr,#E781 → 67h       ; buf[1] = 쓰기 길이   (I2C 루틴이 67h만큼 씀)
inc dptr (E782) → 6bh      ; buf[2] = 읽기 길이
r1=#84h                    ; 쓰기 데이터는 buf[4]부터
lcall X11b8                ; I2C write → repeated start → read, 결과를 E7C1부터
mov E7C0, r7               ; 응답[0] = 상태
mov E68F, E782+1           ; 응답 길이 = rlen+1
```
I2C 루틴에서 `add a,acc`(주소 × 2)가 보이면 **7-bit 주소**를 받는다는 뜻입니다.
상태 코드는 루틴 끝의 `mov r6,#6`(버스 오류), `#7`(NACK), 정상 경로 `#8`로 알아냈습니다.

## 4. 이 장치의 결과
| cmd | 형식 | 응답 |
|---|---|---|
| 0x08 | `08 addr len data…` | `st` (8=OK, 7=NACK, 6=bus error) |
| 0x09 | `09 wlen rlen addr wdata…` | `st data…` |
| 0x0c | `0c addr count chunklen data…` | 여러 번 쓰기 |
| 0x0d | `0d port` (0=PA, 3=PD) | `val` |
| 0x0e | `0e port OE val` | `st` |
| 0x36 | `36 03 00 clk` → IFCONFIG(clk=1: 0xCB 내부 IFCLK·async, 0: 0x03 외부) + FIFO 리셋 + EP2 AUTOIN | 없음 |
| 0x37 | `37` 스트림 정지 | 없음 |

스트림 시작/정지 명령의 인자는 펌웨어만으로는 알 수 없어서 Windows 드라이버에서 확인했습니다(04).

## 5. 펌웨어 올리기 (테스트)
```python
w = lambda a, data: d.ctrl_transfer(0x40, 0xA0, a, 0, data)
w(0xE600, b'\x01')
for a in range(0, len(fw), 64): w(a, fw[a:a+64])
w(0xE600, b'\x00')        # 실행 → 재연결 → usb.core.find() 다시
```
RAM에만 올라가므로 USB를 뽑으면 사라집니다(EEPROM을 건드리지 않음).
