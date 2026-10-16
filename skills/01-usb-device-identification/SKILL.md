---
name: usb-device-identification
description: 알 수 없는 USB 장치(특히 TV 수신기·캡처 장치)의 VID/PID와 USB 디스크립터로 브리지 칩을 추정하고, 리눅스 커널이 이미 지원하는지 확인한다. 리눅스 드라이버가 없는 USB 장치를 처음 조사할 때 사용.
---

# USB 장치 식별

## 목적
드라이버를 만들기 전에 (1) 커널이 이미 지원하는지, (2) 어떤 브리지 칩인지 추정합니다.

## 절차

1. **장치 정보 확인**
   ```sh
   lsusb                                  # Bus 001 Device 003: ID 1c6c:04c9 Skydigital Inc.
   lsusb -v -d 1c6c:04c9                  # 디스크립터 전체
   sudo dmesg | grep -iE "usb 1-|dvb"     # 연결/재연결 로그, 드라이버 바인딩 여부
   ```
2. **커널 지원 여부**
   ```sh
   grep -i "v1C6Cp04C9" /lib/modules/$(uname -r)/modules.alias   # 없으면 커널 미지원
   ls /dev/dvb                                                  # adapter가 생기지 않음
   ```
   linuxtv.org 위키와 웹에서 `VID:PID`로 검색합니다. 같은 회사의 다른 모델 정보(예: Skydigital의
   다른 모델은 EM2861)는 칩이 다를 수 있으니 참고만 합니다.
3. **엔드포인트 구조로 브리지 칩 추정**

   | 특징 | 추정 |
   |---|---|
   | EP1 OUT/IN + EP2/4/6/8 (bulk 512) | **Cypress FX2 (CY7C68013)**: EP2/4/6/8 FIFO 구조가 특징 |
   | EP 0x81 int, 0x82 iso video, 0x84 TS | Empia em28xx |
   | 연결 직후 disconnect 후 같은 ID로 재연결 | 펌웨어 다운로드 후 renumeration 하는 장치일 수 있음 |
   | iManufacturer/iProduct = 0 | 부트 ROM 상태(펌웨어 미적재)일 가능성 |

   이 장치는 alt setting 1~3에 EP1, EP2, EP4, EP6, EP8이 있어서 FX2로 추정했고, 이후 드라이버 경로명
   `cx25843+cy7c68013`으로 확정했습니다.

4. **펌웨어 적재 상태 확인 (FX2)**: FX2는 펌웨어 RAM을 vendor request 0xA0으로 읽을 수 있습니다
   (읽기 전용, 안전).
   ```python
   import usb.core
   d = usb.core.find(idVendor=0x1c6c, idProduct=0x04c9)
   ram = bytes(d.ctrl_transfer(0xC0, 0xA0, 0x0000, 0, 64))   # 주소 0x0000부터 64바이트
   ```
   0x0000이 `02 xx xx`(LJMP)가 아니라 무작위 값이면 펌웨어가 없는 상태입니다. 이 경우 호스트 드라이버가
   연결할 때마다 펌웨어를 올리는 구조이므로, 펌웨어를 Windows 드라이버에서 찾아야 합니다(02, 03 참고).

## 주의
- `lsusb -v`는 일반 사용자로도 대부분 보이지만, 문자열 디스크립터나 일부 정보는 root가 필요할 수 있습니다.
- 펌웨어를 적재한 뒤에는 디스크립터가 바뀝니다. 이 장치는 적재 후 EP2 IN(0x82)만 남았고,
  alt 0은 bulk(TS), alt 1~6은 iso(아날로그 영상)입니다.
