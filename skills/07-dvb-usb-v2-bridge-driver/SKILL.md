---
name: dvb-usb-v2-bridge-driver
description: 리눅스 dvb-usb-v2 프레임워크로 USB 브리지 드라이버를 작성해 커널의 기존 복조기·튜너 드라이버(lgdt3305, xc5000 등)를 붙이고, 보드와 맞지 않는 커널 드라이버 동작을 ops 래핑으로 보정한다. 새 USB DVB/ATSC 장치를 /dev/dvb로 노출할 때 사용.
---

# dvb-usb-v2 브리지 드라이버

구현: `driver/dvb_usb_skytv.c`. 브리지 드라이버는 USB 쪽(펌웨어, 명령, I2C 중계, 스트림)만 맡고,
복조기·튜너는 커널에 이미 있는 드라이버를 `dvb_attach()`로 붙입니다.

## 헤더
`dvb_usb.h`는 linux-headers에 없습니다(드라이버 내부 헤더). 실행 중인 커널과 같은 태그
(`v7.0`)의 `drivers/media/usb/dvb-usb-v2/dvb_usb.h`, `lgdt3305.h`, `xc5000.h`를 소스 옆에 둡니다.
export 심볼은 `Module.symvers`로 확인합니다(`dvb_usbv2_probe`, `lgdt3305_attach`, `xc5000_attach`).

## dvb_usb_device_properties 채우기
| 콜백 | 이 드라이버 |
|---|---|
| `identify_state` | `udev->descriptor.iManufacturer ? WARM : COLD` |
| `firmware` / `download_firmware` | `dvb-usb-skytv-hdusbmini.fw`를 0xA0으로 업로드 → `RECONNECTS_USB` 반환 |
| `i2c_algo` | `master_xfer`: write+read 쌍 → cmd 0x09, 단독 write → 0x08(61바이트 초과는 XC5000 방식으로 분할), 단독 read → 0x09 wlen=0 |
| `read_config` | 포트 상태 읽기 + 전원 순서 (i2c/adapter 초기화보다 먼저 호출됨) |
| `frontend_attach` | `lgdt3305_attach(&cfg{demod_chip=LGDT3304, i2c_addr=0x59, parallel, falling, gated, valid high, if 6000})` |
| `tuner_attach` | `xc5000_attach(fe, &i2c_adap, &cfg{0x61, if_khz=6000})` |
| `streaming_ctrl` | on: `36 03 00 01`, off: `37` |
| `adapter[0].stream` | `DVB_USB_STREAM_BULK(0x82, 8, 32*512)` |
| `size_of_priv` | 명령 버퍼(DMA 가능해야 하므로 priv 안 kmalloc 메모리), GPIO 상태, 원래 ops 포인터 |

probe 순서(`dvb_usb_core.c`): `probe` → `identify_state` → (COLD면 펌웨어 후 재연결) →
`power_ctrl(1)` → `read_config` → i2c 초기화 → adapter 초기화(frontend/tuner attach) → `init`.

## 튜너 리셋 콜백
xc5000은 펌웨어 시퀀스 중 RESET에서 `fe->callback(fe->dvb->priv, DVB_FRONTEND_COMPONENT_TUNER,
XC5000_TUNER_RESET, 0)`을 부릅니다. dvb-usb-v2에서 `fe->dvb->priv`는 `struct dvb_usb_adapter *`이므로
`adap_to_d()`로 장치를 얻어 GPIO를 펄스합니다.

## 커널 드라이버 동작 차이를 ops 래핑으로 보정
커널 `lgdt3305.c`의 LGDT3304 경로는 다른 제품(KWorld 340U) 기준이라 이 보드와 네 군데가 맞지 않았습니다.
커널 모듈을 고치지 않고 `frontend_attach`에서 이 프론트엔드 인스턴스의 ops만 감쌌습니다.

| 문제 | 증상 | 보정 |
|---|---|---|
| VSB에서 CR_CTR_FREQ에 고정값 0x4f0cacba 기록 | 6 MHz IF에서 lock 실패 | `set_frontend` 후 0x0106~0x0109 = 0 |
| init 테이블의 TP_CTRL_1=0x5b, 극성 함수는 bit6을 지우지 않음 | lock은 되지만 TS 깨짐 | `set_frontend` 후 0x050e = 0x1b, soft reset |
| `sleep()`이 AGC 핀 tristate, ADC/VSB off인데 3304 init이 복구 안 함 | 한 번 닫은 뒤 모든 채널 lock 실패 | `init` 래핑: 0x0002 &= ~0x06, 0x000a \|= 0x03 |
| `info.frequency_max_hz` = 858 MHz | 861 MHz(CATV 135) 튜닝 거부 | `ops.info.frequency_max_hz = 1002 MHz` (XC5000은 1023 MHz까지) |

`ops`는 attach 때 인스턴스별로 복사되므로(`memcpy(&state->frontend.ops, …)`) 바꿔도 다른 장치에 영향이 없습니다.
dvb-usb-v2 코어는 attach 뒤 `ops.init`/`sleep`을 한 번 더 감싸므로, 래핑은 반드시 `frontend_attach` 안에서 합니다.

## 디버깅
- `dmesg`: "found a '…' in warm state", "registering adapter 0 frontend 0 (LG Electronics LGDT3304 …)",
  "xc5000: Firmware … loaded and running"
- 연속 테스트(열기 → 닫기 → 다시 열기)를 꼭 합니다. sleep 버그는 **두 번째 열기부터** 나타났습니다.
- `tslsdvb -v`로 프론트엔드가 보고하는 주파수 범위와 기능을 확인합니다.
