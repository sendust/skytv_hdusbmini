---
name: kernel-module-build-dkms
description: 커널 트리 밖(out-of-tree) DVB 드라이버 모듈을 빌드·설치하고 펌웨어를 배치한 뒤, DKMS에 등록해 커널 업데이트 때 자동으로 다시 빌드되게 한다. 직접 만든 리눅스 커널 모듈을 Ubuntu 서버에 영구 설치할 때 사용.
---

# 외부 커널 모듈 빌드와 DKMS

## 빌드
```make
obj-m := dvb_usb_skytv.o
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
```
- `linux-headers-$(uname -r)` 필요. "compiler differs" 경고는 문자열만 다른 경우 무시해도 됩니다.
- vmlinux가 없으면 "Skipping BTF generation" 메시지가 나옵니다. 정상입니다.
- 의존 모듈은 직접 로드: `modprobe dvb_usb_v2 lgdt3305 xc5000` 후 `insmod dvb_usb_skytv.ko`
  (설치 후에는 `modprobe`가 `depends:`를 따라 자동으로 로드)

## Secure Boot
`mokutil --sb-state`, `cat /sys/kernel/security/lockdown`. 이 서버는 EFI 변수가 없고 lockdown이 `none`이라
서명하지 않은 모듈도 로드됐습니다. Secure Boot가 켜진 시스템에서는 MOK 키로 서명해야 합니다.

## 펌웨어 배치
```sh
sudo cp fw.bin /lib/firmware/dvb-usb-skytv-hdusbmini.fw       # MODULE_FIRMWARE 이름과 같게
```
XC5000 펌웨어(`dvb-fe-xc5000-1.6.114.fw`)는 `linux-firmware` 패키지에 들어 있습니다.

## DKMS
`dkms.conf`:
```sh
PACKAGE_NAME="dvb-usb-skytv"
PACKAGE_VERSION="1.0"
BUILT_MODULE_NAME[0]="dvb_usb_skytv"
DEST_MODULE_LOCATION[0]="/updates/dkms"
MAKE[0]="make KDIR=${kernel_source_dir}"
CLEAN="make KDIR=${kernel_source_dir} clean"
AUTOINSTALL="yes"
```
```sh
sudo mkdir -p /usr/src/dvb-usb-skytv-1.0
sudo cp dvb_usb_skytv.c dvb_usb.h lgdt3305.h xc5000.h Makefile dkms.conf /usr/src/dvb-usb-skytv-1.0/
sudo dkms add dvb-usb-skytv/1.0
sudo dkms install dvb-usb-skytv/1.0 -k $(uname -r)
sudo dkms install dvb-usb-skytv/1.0 -k 7.0.0-34-generic     # 설치만 되어 있는 다른 커널도
dkms status
```
소스를 고친 뒤:
```sh
sudo cp dvb_usb_skytv.c /usr/src/dvb-usb-skytv-1.0/
sudo dkms unbuild dvb-usb-skytv/1.0 -k $(uname -r); sudo dkms install dvb-usb-skytv/1.0 -k $(uname -r) --force
sudo rmmod dvb_usb_skytv; sudo modprobe dvb_usb_skytv
```

## 주의
- `ls /boot/vmlinuz-*`로 설치된 다른 커널을 확인하세요. 다음 부팅 커널에 모듈이 없으면 장치가 사라집니다.
- 수동으로 `updates/`에 복사한 `.ko`와 DKMS 모듈이 함께 있으면 어느 쪽이 로드될지 헷갈립니다. 하나만 남기고 `modinfo -n`으로 확인합니다.
- 헤더(`dvb_usb.h`)는 커널 메이저 버전이 바뀌면 다시 받아야 할 수 있습니다(구조체가 바뀌면 빌드는 되어도 동작이 깨질 수 있음).
