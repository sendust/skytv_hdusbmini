---
name: userspace-prototyping-pyusb
description: 커널 모듈을 쓰기 전에 pyusb로 USB 수신기를 직접 제어해(펌웨어 업로드, GPIO, I2C, 튜너 펌웨어 로드, 튜닝, TS 수신) 설정값을 검증한다. 새 DVB/ATSC USB 장치의 드라이버를 개발할 때 커널 패닉 위험 없이 빠르게 실험하려고 사용.
---

# pyusb 사용자 공간 프로토타입

구현: `tools/skytv.py` (이 프로젝트). 커널 드라이버가 로드되어 있으면 장치를 잡고 있으므로 먼저
`sudo rmmod dvb_usb_skytv`를 실행한 뒤 테스트합니다.

## 왜 먼저 사용자 공간인가
- 재컴파일·재부팅 없이 값을 바꿔 가며 시험할 수 있습니다(IF, 스펙트럼 반전, 클럭 모드 등).
- 커널 드라이버와 "같은 레지스터 순서"를 흉내 내서 커널 쪽 문제도 미리 찾을 수 있습니다(`kpath.py` 방식).

## 구성 요소
1. **장치 열기와 펌웨어**: iManufacturer 문자열로 펌웨어 적재 여부 확인 → 없으면 0xA0 업로드 → 재연결 대기
2. **명령 함수**: EP1 OUT 쓰기 → EP1 IN 읽기. 첫 응답이 가끔 유실되므로 짧은 타임아웃(300~500 ms)과 재전송
3. **GPIO**: 포트별 OE/VAL 상태를 기억하고 비트 단위로 갱신 (`0e port OE val`)
4. **전원 순서**: PD6, PD1(튜너) → PD7 펄스(복조기)
5. **XC5000 펌웨어 로더**: `/lib/firmware/dvb-fe-xc5000-1.6.114.fw` 형식
   - 2바이트 길이 `0x0000` = RESET(콜백 → PD1 펄스), `0x8000|ms` = WAIT, 나머지 = I2C 쓰기 데이터
   - 끝은 `FF FF`. 쓰기는 64바이트 이하로 나누고 **앞 2바이트(주소 헤더)를 매 조각 반복**
   - 끝나면 `XREG_INIT(0x00)=0`, product id 0x1388 확인
6. **튜닝**: signal source(air=0) → VIDEO_MODE=0x8002, AUDIO_MODE=0x00C0 → IF_OUT=6000×1024/1000
   → OUTPUT_AMP=0x8a → FINERFREQ=(freq − 1.75 MHz)/15625
7. **복조기**: Windows VSB 테이블 → soft reset(0x0002 bit0) → `0x0003`/`0x011d`로 lock 판정, `0x0413~5` MSE
8. **스트림**: `36 03 00 01` → EP2 bulk 읽기 → 188바이트마다 0x47 확인 → `37`

## 검증 기준
| 단계 | 기준 |
|---|---|
| 튜너 | XREG_LOCK(0x04)=1 |
| 복조기 | GEN_STATUS sync, CR lock(0x011d bit7), MSE 작음(수십) |
| TS | 약 19.39 Mbps, 0x47 정렬, `tsp -P continuity`로 오류 없음 |

## 실제로 디버깅한 것
- 모든 채널 "no lock" → XC5000 VIDEO/AUDIO 모드를 반대로 넣었음
- lock은 되지만 TS 0바이트 → 외부 IFCLK 모드(`36 03 00 00`) 대신 내부 클럭(`… 01`)
- 0x47 비율 94.6% → 검사 함수가 첫 오프셋만 기준으로 세던 버그. 재동기 방식으로 다시 세니 시작 부분 3곳만 끊김
- 커널 경로 재현(`kpath.py`): 고정 NCO 때문에 lock 실패, TP_CTRL_1=0x5b 때문에 TS 깨짐 → 07의 보정으로 연결

## 주의
- 장치 열 때 `set_interface_altsetting(0, 0)`(bulk TS). alt 1~6은 iso(아날로그).
- `pkill -f 패턴`은 같은 패턴이 들어간 자기 셸까지 죽일 수 있습니다. PID를 정확히 골라 `kill`하세요.
