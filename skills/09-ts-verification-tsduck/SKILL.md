---
name: ts-verification-tsduck
description: dvbv5-zap, dvbv5-scan, TSDuck(tsp, tsscan, tslsdvb), ffprobe로 DVB/ATSC 수신기의 lock, 비트레이트, TS 연속성, PCR, PSIP 테이블, 실제 영상 디코딩까지 검증한다. 새로 만든 수신기 드라이버가 제대로 동작하는지 확인할 때 사용.
---

# 수신과 TS 검증

## 단계별 기준
| 단계 | 도구 | 합격 기준 |
|---|---|---|
| 장치 인식 | `tslsdvb -v` | adapter, 지원 시스템(ATSC), 주파수 범위 |
| lock | `dvbv5-zap -x` 또는 `tsp -I dvb` | status 0x1f / "locked: yes" |
| 비트레이트 | `tsp -P bitrate_monitor` | ATSC 8-VSB 19,392,658 b/s |
| TS 무결성 | `tsp -P continuity`, `-P analyze` | invalid sync 0, CC 오류 0 (시작 직후 제외) |
| 시계 | `tsp -P pcrverify` | 지터 1 ms 초과 0건 |
| 내용 | `ffprobe`, `ffmpeg -frames:v 1` | MPEG-2 1080i, AC-3, 실제 프레임 |
| PSIP | `tsp -P tables` | MGT, TVCT, STT, EIT, ETT |

## 자주 쓴 명령
```sh
tslsdvb -v
tsp -I dvb --adapter 0 --delivery-system ATSC --frequency 479000000 --modulation 8-VSB \
    -P until --seconds 30 -P continuity -P pcrverify -P bitrate_monitor --periodic-bitrate 10 \
    -P analyze -o an.txt -O file rec.ts
ffmpeg -v error -ss 6 -i rec.ts -frames:v 1 -vf yadif,scale=640:-2 snap.jpg
```
전체 예시는 프로젝트 `README.md`의 "TSDuck 사용 예시"를 참고하세요.

## 해석 요령
- 녹화 파일 시작 부분이 패킷 경계와 맞지 않으면 `tsp -I file`이 "cannot detect TS file format"으로 실패합니다.
  이때는 재동기하며 정렬된 패킷만 다시 쓰는 `tools/tsclean.py`를 씁니다.
- 시작 직후 수백 패킷 안에서, 그것도 존재하지 않는 PID에서만 CC 오류가 나면 스트림 시작 시점의 잔여 데이터입니다.
- `analyze`의 `ISDB NBIT/LDT` 같은 표기는 PID 용도 자동 추정이 틀린 것이라 무시해도 됩니다.
- 이 드라이버(lgdt3305)는 DVBv5 통계를 제공하지 않아서 TSDuck dvbstatus에 SNR/세기가 나오지 않습니다.
  `dvbv5-zap`은 DVBv3 값으로 대신 보여줍니다(`C/N` %는 0.1 dB 단위 값을 퍼센트로 잘못 표시한 것).
- **EIT/ETT PID는 MGT가 정하며 방송사마다 다릅니다**(KBS 0x1D00~, SBS 0x1A00~). PID를 고정하지 말고
  PID 필터 없이 `tables`를 돌리거나 MGT를 먼저 읽으세요.
- `tsscan`의 korea 지역에는 UHF 정의가 없습니다. 한국은 미국과 같은 6 MHz 채널표라 `--usa`를 씁니다.
- STT `system_time`은 GPS 초(1980-01-06 기준)입니다. UTC = GPS epoch + (system_time − gps_utc_offset).
- ETT `etm_id` = `source_id << 16 | event_id << 2 | 2` → EIT 이벤트의 상세 설명과 연결합니다.
