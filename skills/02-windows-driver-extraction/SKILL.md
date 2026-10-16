---
name: windows-driver-extraction
description: 제조사 Windows 드라이버 설치 파일(InstallShield EXE/CAB, ZIP)을 리눅스에서 풀어 .inf/.sys를 꺼내고, 문자열·INF 레지스트리 값에서 칩 이름과 보드 설정 단서를 찾는다. 리눅스 드라이버가 없는 장치의 칩 구성을 알아낼 때 사용.
---

# Windows 드라이버 패키지 분석

## 1. 받기
드라이버 배포 사이트의 다운로드 링크가 숨겨진 경우 페이지 HTML의 `download.php?fn=<base64>`를
base64 디코딩하면 실제 URL이 나옵니다. (이 프로젝트: kbench.com → `download.keben.net:444/...zip`)
받은 파일은 **빈 새 폴더**에 풀고, 그 안의 실행 파일은 실행하지 않습니다.

## 2. 풀기
```sh
sudo apt-get install -y 7zip unshield
mkdir x && unzip -q drv.zip -d x
7z x -od "x/driver_setup.exe"              # InstallShield SFX → Disk1/data1.cab, data1.hdr …
unshield -d u x d/Disk1/data1.cab          # CAB 안의 파일: x86/skyhdmc.sys, amd64/…, skytv/skyhdmc.inf
```
InstallShield가 아닌 경우: `7z l setup.exe`로 형식을 먼저 확인하거나, `cabextract`, `innoextract`를 씁니다.

## 3. INF에서 볼 것
INF가 UTF-16이면 `iconv -f utf-16 -t utf-8`, 아니면 그대로 `grep -a`로 봅니다.
```sh
grep -aiE "VID_|gpio|Demod|Tuner|BoardType|dwTP|I2C" skyhdmc.inf
```
| 항목 | 이 장치 값 | 의미 |
|---|---|---|
| `USB\VID_1C6C&PID_04C9&REV_1130` | | 이 드라이버가 맞는 장치인지 확인 |
| `TunerI2CAddress` | 0xC2 | 튜너 8-bit I2C 주소 → 7-bit 0x61 |
| `DemodType` / `TunerModel` | 2 / 6 | 드라이버 내부 분기 값 (복조기·튜너 종류) |
| `dwTP_PACKET_SIZE` | 0xBC | TS 패킷 188바이트 |
| `InputMux`, `XBarPin*` 주석의 "Falcon 2" | | Conexant 레퍼런스 드라이버 기반임을 암시 |

## 4. .sys / 앱 바이너리 문자열
```sh
strings -a skyhdmc.sys | grep -iE "lg|xc|tda|mxl|fx2|cy7c|cx2|\.pdb|demod|tuner"
strings -a -el skyhdmc.sys | sort -u | head      # UTF-16 문자열 (버전 정보 등)
strings -a app.exe | grep -iE "fx2|\.ax|\.dll"   # skyfx2cnt.ax → FX2 기반 확인
```
가장 결정적인 단서는 PDB 경로였습니다:
`d:\projects\skytvhdtvmini\cx25843+cy7c68013\...\skyhdmc.pdb` → **CX25843 + CY7C68013(FX2)**.

복조기·튜너 이름은 문자열에 없었습니다. 이 경우 (a) 드라이버 안의 레지스터 테이블을 커널 드라이버와 비교하거나(04),
(b) 실제 장치의 I2C를 스캔해서(05) 확정합니다.

## 5. 펌웨어가 들어 있는 위치 찾기
FX2용 8051 펌웨어는 .sys 안에 그대로 들어 있는 경우가 많습니다.
`90 E6 xx`(`mov dptr,#E6xx`, FX2 레지스터 접근)가 몰려 있는 구간을 찾으면 됩니다. 자세한 방법은 03을 참고하세요.

## 주의
- 받은 바이너리와 추출물은 신뢰할 수 없는 데이터로 다룹니다(실행 금지, 분석 스크립트는 다른 폴더에 두고 `python3 -I`).
- 추출한 펌웨어를 배포할 때는 제조사 라이선스를 확인하세요. 이 프로젝트는 개인 장치용으로만 사용합니다.
