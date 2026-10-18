# dji-cellular-dongle-mac (한국어 안내)

**DJI 셀룰러 동글 2**(USB `2ca3:4009`, 내부 모뎀은 Fibocom NL668로 보고)를 제조사 드라이버 없이 macOS의 LTE 모뎀으로 씁니다.

동글을 꽂으면 맥이 LTE로 인터넷에 연결되고, 빼면 원래 네트워크로 돌아갑니다.

> **상태: 실험 단계입니다.** 맥 1대(Apple Silicon, macOS 26.6), 동글 1개, SIM 1개로만 시험했습니다. root로 실행되고, 동글이 꽂혀 있는 동안 맥의 기본 경로와 DNS를 바꿉니다. 설치 전에 [위험](#위험)을 읽어 주세요.

DJI와 관계없는 비공식 프로젝트이며, DJI가 보증하지 않습니다. DJI는 SZ DJI Technology Co., Ltd.의 상표입니다.

[English README](README.md)

## 동작 방식

- `ppp_lte_up`(root)은 커널 확장 없이 사용자 공간(IOKit)에서 동글의 AT/PPP 인터페이스를 열고, 데이터 접속을 건 뒤 macOS 기본 `pppd`(`/usr/sbin/pppd`)를 그 위에서 실행합니다.
- `pppd`가 맥 전체의 기본 경로와 통신사 DNS를 설치합니다(`defaultroute usepeerdns`). 이 정보는 고정 SystemConfiguration 서비스 ID 하나에 게시됩니다. LCP echo로 끊긴 링크를 약 40초 안에 알아챕니다.
- 세션이 어떤 이유로 끝나도(Ctrl+C, 분리, 링크 끊김) 그 고정 서비스 ID의 남은 키를 지웁니다. 그러면 macOS가 Wi-Fi나 유선으로 돌아갑니다.
- `ppp_lte_upd`는 작은 launchd 데몬입니다. 동글을 기다렸다가 세션마다 `ppp_lte_up --up`을 새 자식 프로세스로 실행합니다. 꽂혀 있는 동안 끊기면 5초부터 두 배씩 늘려 최대 5분 간격으로 다시 연결합니다.

## 필요한 것

- `/usr/sbin/pppd`가 있는 macOS(현재 macOS에 기본 포함)와 Xcode Command Line Tools(`xcode-select --install`)
- 데이터 요금제가 있는 SIM을 넣은 DJI 셀룰러 동글 2
  - APN은 모뎀에 미리 저장돼 있어야 합니다(보통 동글 초기 설정에서 저장됩니다).
  - 이 도구는 APN·PIN·USB 모드를 바꾸지 않습니다.
- PPP 설정 파일이 없어야 합니다. `/etc/ppp/` 아래 파일(options, ip-up 등)이나 `/var/root/.ppprc`가 있으면 세션이 시작을 거부합니다.
- 다른 `pppd`(예: L2TP VPN)가 실행 중이면 안 됩니다.

## 빌드와 시험

```sh
make                 # build/ppp_lte_up, build/ppp_lte_upd 빌드
make verify-offline  # fake로 모든 단위·통합 시험(장치·root·네트워크 없음)
```

## 설치 없이 한 번 써 보기

```sh
sudo build/ppp_lte_up --up
```

`up=connected ...`가 나오면 인터넷을 쓰고, Ctrl+C로 멈춥니다. 마지막 줄이 `up-result=stopped ...`이고 종료 코드가 0이면 깨끗이 정리된 것입니다. `next-run-cleanup=required`로 끝나면 `sudo build/ppp_lte_up --cleanup`을 실행하세요.

## 설치 (꽂으면 바로 연결)

```sh
make
sudo make install
```

설치되는 파일은 다음과 같습니다.

| 경로 | 내용 |
|---|---|
| `/Library/PrivilegedHelperTools/dji-cellular-dongle-mac/ppp_lte_up`, `ppp_lte_upd` | 실행 파일(root:wheel) |
| `/Library/LaunchDaemons/io.github.henryj-dev.dji-cellular-dongle-mac.plist` | launchd 작업(부팅 시 시작) |
| `/etc/newsyslog.d/dji-cellular-dongle-mac.conf` | 로그 회전 설정 |
| `/var/log/dji-cellular-dongle-mac.log` | 로그(고정 라벨과 숫자만) |

macOS가 "백그라운드 항목 추가" 알림을 띄웁니다. 시스템 설정 > 일반 > 로그인 항목에서 이 항목을 끄면 데몬이 멈춥니다.

제거(로그 파일은 남깁니다):

```sh
sudo make uninstall
```

## 동작

| 상황 | 결과 |
|---|---|
| 동글 꽂기 | 약 6~10초 안에 연결, 맥 전체 트래픽이 LTE로 |
| 동글 빼기 | 세션 정리, 남은 키 삭제, 이전 네트워크로 복귀 |
| 꽂힌 채 끊김 | 정리 후 5초, 10초, … 최대 5분 간격으로 재연결 |
| 시도가 바로 실패하는 일이 3번 연속(강제 종료 뒤 모뎀이 데이터 모드에 갇힘) | `upd=replug-required`를 남기고 대기. 동글을 뺐다가(1초 이상) 다시 꽂으면 재개 |
| 동글 2개 | 대기(1개만 지원) |
| `launchctl bootout`·시스템 종료 | 세션을 정상 종료(최대 약 60초)한 뒤 정리 |

데몬은 실행할 때마다 자식 실행 파일과 그 위 모든 디렉터리가 root 소유이고 다른 사용자가 쓸 수 없는지 확인합니다.

## 지금까지 확인한 것

시험한 맥(Apple Silicon, macOS 26.6)에 데몬을 설치하고 확인한 결과입니다.

- 꽂기: 약 7초 만에 연결, 기본 경로 `ppp0`, HTTPS 동작
- 빼기: 세션 정리, 남은 키 삭제, Wi-Fi가 다시 primary
- 다시 꽂기: 자동 재연결
- `adb` 서버가 켜져 있어도 연결 유지(`children-check tolerated creator=adb`)
- `sudo make uninstall`: 데몬과 파일 제거
- `networkQuality` 실측: **다운로드 약 3.6 Mbps, 업로드 약 11 Mbps, 유휴 지연 약 70ms** (첫 공개 때 1.6 / 8 Mbps, 270ms)
  - 중계는 이제 USB 읽기 4개를 미리 걸어 두고 초당 약 700바퀴 돕니다. 그래도 데이터가 읽는 속도보다 느리게 들어옵니다.
  - 남은 한계는 시험한 자리의 LTE 속도이거나 모뎀의 PPP(다이얼업) 모드일 가능성이 큽니다. 같은 SIM을 넣은 휴대폰이 같은 자리에서 더 빠르고 요금제 속도 제한도 없어서, 모뎀의 PPP 모드가 한계일 가능성이 큽니다. 세션마다 다이얼 전에 `up-signal ...` 한 줄(CSQ RSSI, LTE RSRP·RSRQ)을 로그에 남깁니다. 자세한 내용은 `docs/ppp-design.md`를 보세요.
- 시험하지 않은 것: 잠자기·깨우기, 몇 시간 이상 긴 세션, 다른 macOS 버전이나 다른 맥

## 위험

- **동글이 꽂혀 있는 동안 모든 트래픽이 LTE로 나갑니다.** 백그라운드 업데이트와 클라우드 동기화도 포함되니 데이터 요금제를 확인하세요.
- 연결 중 IPv6는 막힐 것으로 예상합니다. 모뎀이 IPCP를 재협상하는 순간에는 짧게 Wi-Fi로 나갈 수 있습니다.
- root로 실행되고 시스템 `pppd`를 씁니다. 전원 차단처럼 강제로 끝나면 SystemConfiguration 키가 남을 수 있습니다. 다음 시작 때 자동으로 지우고, 직접 지우려면 `sudo /Library/PrivilegedHelperTools/dji-cellular-dongle-mac/ppp_lte_up --cleanup`을 실행합니다.
- 실행 파일은 직접 빌드한 ad-hoc 서명입니다. 공증된 설치 패키지는 아직 없습니다.
- PPP 원격 종료와 통신사 PDP 해제는 명시적으로 하지 않습니다. 모뎀에 맡깁니다.

## 제한

- 한 가지 동글 모델·인터페이스 구성만 지원합니다. 다른 모뎀은 거부합니다.
- 네트워크 순서를 지정할 수 없습니다. 꽂혀 있으면 항상 LTE가 우선합니다. 동글을 일반 모뎀 포트로 보이게 하는 DriverKit 시리얼 드라이버가 있으면 순서 지정이 가능하지만 아직 구현하지 않았습니다.
- 다른 기기와 인터넷 공유(NAT), SMS, APN·PIN 설정은 없습니다.
- 잠자기·깨우기 뒤 동작은 시험하지 않았습니다.
- 세션은 동글 인터페이스를 다른 프로세스와 함께 쓰지 않습니다. 시험한 맥에서는 Android Debug Bridge 서버(`adb`, Android Studio 등)가 이 인터페이스에 user client를 계속 하나 더 만들었습니다. 세션이 독점으로 열고 있는 동안에는 adb가 송수신을 못 하므로, 이 하나는 이제 허용합니다. 다른 프로세스가 같은 일을 하면 여전히 세션을 멈추고 로그에 `children-check reason=extra-child ... creator=<이름>`을 남깁니다.

## 저장소의 다른 도구

개발 중에 쓴 도구들입니다.
- `build/dji-lte-mac`: 설명자 진단과 고정 읽기 전용 AT 조회
- `build/ppp_lte_once`: 고정 echo 시험 1회
- `build/ppp_auth_trial`: 동글 없이 로컬 pppd 시험

개발 기록은 [docs/history/](docs/history/)에, 설계는 [docs/ppp-design.md](docs/ppp-design.md)에 있습니다.

## 라이선스

MIT, [LICENSE](LICENSE)를 보세요.
