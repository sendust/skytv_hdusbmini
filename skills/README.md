# SKYTV HD USB mini 리눅스 드라이버 제작에 쓴 기술 (skills)

제조사가 Windows 드라이버만 제공하는 USB TV 수신기를, 칩 구성부터 알아내서 리눅스 DVB 드라이버로 만들기까지
필요했던 기술을 단계별로 정리했습니다. 각 폴더의 `SKILL.md`는 Claude Code 스킬 형식(name/description 머리말)을 따르므로
사람이 읽는 문서로도 쓰고, `~/.claude/skills/`나 프로젝트의 `.claude/skills/`에 복사하면 Claude Code 스킬로도 쓸 수 있습니다.

## 전체 흐름

| 단계 | 스킬 | 이 프로젝트에서 얻은 것 |
|---|---|---|
| 1 | [usb-device-identification](01-usb-device-identification/SKILL.md) | VID/PID, 엔드포인트 구조로 Cypress FX2라고 추정, 커널 미지원 확인 |
| 2 | [windows-driver-extraction](02-windows-driver-extraction/SKILL.md) | InstallShield 풀기, INF·.sys에서 `cx25843+cy7c68013`, DemodType, IF 등 단서 |
| 3 | [fx2-firmware-reverse-engineering](03-fx2-firmware-reverse-engineering/SKILL.md) | 펌웨어 추출, 8051 디스어셈블, EP1 명령 테이블 (I2C, GPIO, 스트림) |
| 4 | [windows-sys-reverse-engineering](04-windows-sys-reverse-engineering/SKILL.md) | GPIO 전원 순서, 복조기 레지스터 테이블, 튜너 IF 6 MHz, 리셋 콜백 |
| 5 | [i2c-probing-chip-identification](05-i2c-probing-chip-identification/SKILL.md) | I2C 스캔, LGDT3304(0x59)·XC5000(0x61) 확정 |
| 6 | [userspace-prototyping-pyusb](06-userspace-prototyping-pyusb/SKILL.md) | 커널 없이 튜닝·lock·TS 수신 검증 (`tools/skytv.py`) |
| 7 | [dvb-usb-v2-bridge-driver](07-dvb-usb-v2-bridge-driver/SKILL.md) | 커널 모듈 `dvb_usb_skytv`, 커널 드라이버 동작 차이 보정 |
| 8 | [kernel-module-build-dkms](08-kernel-module-build-dkms/SKILL.md) | 외부 모듈 빌드, 펌웨어 설치, DKMS 자동 재빌드 |
| 9 | [ts-verification-tsduck](09-ts-verification-tsduck/SKILL.md) | dvbv5-zap / TSDuck / ffprobe로 lock·TS·PSIP 검증 |

## 사용한 도구

| 분류 | 도구 |
|---|---|
| USB | `lsusb -v`, `dmesg`, `/lib/modules/$(uname -r)/modules.alias`, pyusb (`python3-usb`) |
| 압축 해제 | `7z` (`7zip`), `unshield` (InstallShield CAB), `unzip` |
| 바이너리 분석 | `strings` (`-el` 포함), `xxd`, `objdump -d -M intel` (PE x86), `d52` (8051 디스어셈블러) |
| 커널 소스 | git.kernel.org plain 파일 (`?h=v7.0`): `lgdt3305.c`, `xc5000.c`, `dvb_usb.h`, `dvb_usb_core.c` |
| 빌드 | `linux-headers-$(uname -r)`, `make -C /lib/modules/$(uname -r)/build M=$PWD`, `dkms` |
| 검증 | `dvbv5-zap`, `dvbv5-scan`, TSDuck (`tsp`, `tsscan`, `tslsdvb`), `ffprobe`/`ffmpeg` |

## 이번에 실제로 빠졌던 함정 (요약)

1. **I2C 주소 형식**: FX2 펌웨어가 내부에서 `addr << 1`을 하므로 7-bit 주소를 넘겨야 함. 8-bit로 보내면 전부 NACK(0x07).
2. **명령 패킷 필드 순서**: cmd 0x09는 `[09, wlen, rlen, addr, wdata…]`. 디스어셈블에서 어떤 버퍼 오프셋이
   어떤 변수로 가는지(`E781`→wlen, `E782`→rlen, `E783`→addr) 한 줄씩 추적해야 했음.
3. **전원이 꺼진 칩은 I2C에 안 보임**: 처음 스캔에서는 CX25843과 EEPROM만 보였음. Windows 드라이버의 GPIO 순서
   (PD6/PD1 → 튜너, PD7 리셋 → 복조기)를 재현한 뒤에야 0x61, 0x59가 나타남.
4. **XC5000 비디오/오디오 모드 순서**: 커널 표 `{name, audio_mode, video_mode}`를 반대로 읽어서 lock 실패.
   VIDEO=0x8002, AUDIO=0x00C0이 맞음.
5. **커널 lgdt3305의 LGDT3304 경로가 이 보드와 안 맞음**: VSB 고정 NCO, TP_CTRL_1 bit6이 남는 문제,
   sleep 후 AGC/ADC를 다시 켜지 않는 문제, 최대 주파수 858 MHz. 모두 브리지 드라이버에서 보정.
6. **스트림 클럭 모드**: `[36 03 00 00]`(외부 IFCLK)은 데이터 0바이트, `[36 03 00 01]`(내부 IFCLK)에서 정상.
7. **FX2 EP1 버퍼 64바이트**: XC5000 펌웨어 청크(최대 64바이트)를 그대로 못 보냄. 2바이트 주소 헤더를 반복해서 분할.
8. **세션 시작 직후 첫 응답 유실**: EP1 응답이 가끔 안 옴. 짧은 타임아웃 + 재전송으로 해결.
