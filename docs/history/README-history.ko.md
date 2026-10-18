# 개발 기록 (한국어)

> 이 문서는 개발 중 실장치 시험과 설계 결정을 시간 순으로 남긴 기록입니다. 글에 나오는 「승인」·「독립 검토」·「coordinator」는 당시 작업 절차(실기마다 사용자 승인, 별도 코드 검토)를 가리킵니다. 사용 방법은 저장소 루트의 `README.md`(영문) 또는 `README.ko.md`(한국어)를 보세요.

## DJI LTE 동글 USB 진단

생산 앱은 캐시 설명자 진단과 명시적인 제한 BULK IN 관찰 도구입니다. 별도 LTE 단발 실행기(`build/ppp_lte_once`)는 2026-10-17 열 번째 실기에서 처음으로 동글의 LTE 경로로 인터넷 서버와 송수신에 성공했습니다(아래 「열 번째 실기 1회 결과」). 맥 전체가 동글로 인터넷을 쓰는 연결 유지 실행기(`build/ppp_lte_up`)는 2026-10-17 실기에서 맥 전체 기본 경로를 동글로 바꿔 HTTPS 응답을 받고 Ctrl+C로 깨끗이 정리하는 데 성공했습니다(아래 「연결 유지 모드 실기」). 실행할 때마다 별도 승인이 필요합니다. 인터넷 공유는 구현하지 않습니다. 프로토콜·칩셋·공개 드라이버 대응 여부는 미확정입니다.

## 연결 유지 모드: 구현과 오프라인 검증만

`build/ppp_lte_up`은 단발 실행기와 따로 만든 연결 유지 실행기입니다. 명령 하나로 LTE PPP 연결을 열어 두고, 그동안 **맥 전체의 기본 경로와 DNS가 동글로 바뀝니다**. 실행할 때마다 별도 승인이 필요합니다. 꽂으면 자동 연결(launchd)과 다른 기기 공유(NAT)는 구현하지 않습니다.

- CLI: `--up`, `--cleanup`, `--help` 중 하나만 받습니다. 그 밖의 인자는 host 접근 전 exit 2입니다. `--up`과 `--cleanup`은 root가 필요합니다.
- pppd 정책: 단발·auth 정책(`ppp_launch_prepare`)은 그대로 두고, 별도 고정 argv(`ppp_launch_prepare_up`)를 씁니다. `nodefaultroute`를 빼고 `defaultroute usepeerdns`를 넣었습니다. `serviceid`로 고정 서비스 ID 하나에만 게시하게 하고, `lcp-echo-interval 10 lcp-echo-failure 3`으로 링크가 죽으면 pppd가 약 40초 안에 스스로 끝나게 했습니다. `noipv6`는 유지하고 `noipv6override`는 넣지 않았습니다. 공개 소스상 `defaultroute`가 IPv6 더미 primary(`::1`)를 함께 게시해, 연결 중 IPv6 트래픽이 Wi-Fi로 새지 않게 막는 효과를 기대합니다. configd의 실제 반응은 첫 실기에서 확인합니다.
- 세션: session 예산 30일, USB IN timeout은 명시값 75초입니다(단발은 예산 합으로 유도). LCP echo 실패 감지(40초)가 USB timeout보다 먼저 오도록 컴파일 시점에 고정했습니다. 30일에서 10분 전에는 TERM 경로로 먼저 멈춥니다(`session-ceiling`).
- 연결 판정: PPP 서비스 귀속(30초), 주소 3초 안정(60초 안), 그리고 Global IPv4 primary가 이 서비스인지 확인한 뒤 `up=connected` 한 줄을 냅니다. 안정 뒤 10초 안에 primary가 되지 않으면 `primary-not-taken`으로 멈춥니다. 연결 뒤 모뎀의 IPCP 재협상으로 주소가 잠깐 사라지면 `up=address-lost`/`up=address-restored`로 알리고, 120초 넘게 돌아오지 않으면 멈춥니다. 자동 재연결은 없습니다.
- 정지: Ctrl+C·SIGTERM·SIGHUP은 pppd에 TERM을 보내고 최대 약 10초 정리를 기다립니다. pppd가 스스로 끝나거나 동글을 빼면(USB 오류 → KILL) 바로 정리합니다.
- 잔여 상태 정리: 지우는 대상은 고정 서비스 ID의 `State:/Network/Service/<ID>/{PPP,IPv4,IPv6,DNS,Proxies,Interface,SMB}` 일곱 키뿐이고 모두 컴파일 시점 상수입니다. Setup:·Global·다른 서비스(Wi-Fi·VPN)는 쓰지 않습니다. 살아 있는 `pppd`나 다른 `ppp_lte_up`이 있거나 프로세스 목록을 읽지 못하면 아무것도 지우지 않습니다. 정리 시점은 시작 전(관측 기준선 이전), pppd 회수 직후(정리 완료를 기다리지 않음), 종료 직전, 그리고 `--cleanup` 명령입니다. 공개 소스상 pppd는 정상 종료에도 IPv6 더미 키를 남기고, KILL되면 모든 키를 남깁니다. 요약의 `pppd-leftover`가 실제로 남은 키를 보여 줍니다.
- 종료 코드: 0은 사용자 정지 뒤 pppd가 TERM으로 끝나고(exit 5, KILL 없음) 잔여 키 0, primary가 이 서비스에서 벗어났을 때입니다. 전후 SC global 비교는 진단으로만 출력합니다(긴 세션에서는 Wi-Fi 로밍·DHCP로 무관하게 바뀔 수 있어 종료 코드에 넣지 않음). 3은 그 밖의 종료로 정리는 확인된 경우, 4는 정리를 확인하지 못한 경우(`next-run-cleanup=required`, `sudo build/ppp_lte_up --cleanup` 권장)입니다. 원격 LCP·PDP 종료는 여전히 `unproven`입니다.
- 출력: 고정 라벨과 숫자만 냅니다. IP·DNS·서비스 ID·인터페이스 이름·pid는 출력하지 않습니다.
- 오프라인 검증: launch·supervisor·session 정책, SC 정리(fake store·process table), 주소 감시(fake interface table), 실행기 단위(fake runner)와 실제 up+link+service 결합 시험이 `make verify-offline`(일반·ASan/UBSan)에 들어 있습니다. 이 결과는 실제 USB·pppd·LTE·configd 동작의 증거가 아닙니다.

실기 전 확인할 위험: 연결 중 모든 트래픽이 LTE로 나가 과금됩니다. 모뎀 재협상 순간 짧게 Wi-Fi로 새는 창이 있을 수 있습니다. 연결 중 IPv6는 막힐 것으로 예상합니다. 비정상 종료(SIGKILL·전원 차단) 뒤에는 키가 남을 수 있어 `--cleanup`이 필요합니다. 동글 분리 시 USB 콜백이 늦으면 프로세스 종료가 길어질 수 있지만, SC 정리는 pppd 회수 직후에 먼저 합니다. configd가 Setup: 항목이 없는 State 전용 서비스를 primary로 올리는지는 미확인입니다(안 올리면 `primary-not-taken`). pppd는 같은 프로세스 그룹이라 Ctrl+C가 pppd에도 직접 갑니다(그래도 exit 5). 터미널 창을 닫으면 pppd가 SIGHUP으로 다른 코드(16)로 끝나 결과가 3 또는 출력 오류 1이 됩니다. 연결 중 다른 PPP 서비스(L2TP VPN 등)가 생기거나 SC 읽기가 실패하면 `attribution-lost`로 끊습니다. 잠자기 시간도 시계에 포함돼 깨어난 직후 `address-lost-timeout`이 날 수 있습니다. 요약의 `connect-primary-*`는 연결 시점 값이고, `ppp-lcp-echo`/`ppp-lcp-echo-reply`로 모뎀의 LCP echo 응답 여부를 봅니다(응답하지 않으면 약 40초마다 `pppd-exit`).

### 2026-10-17 연결 유지 모드 실기

사용자가 커밋 `065c6da` 빌드로 직접 두 번 실행했습니다.

- 첫 번째: 주소를 받은 뒤(3326ms) 3초 안정 전에 `usb-error`(site=write, kind=other)로 끊겼습니다. USB 쓰기마다 하는 인터페이스 자식 검사(`check_children`)가 "다른 owner/알 수 없는 child"로 실패했습니다. 무엇이 붙었는지는 아직 확인하지 못했습니다. pppd는 KILL됐고 남긴 키 5개(ppp·ipv4·ipv6·dns·proxies)를 회수 직후 지워 잔여 0, primary 복귀 yes였습니다.
- 두 번째: 6369ms에 `up=connected`(IPv4 primary가 이 서비스, IPv6 primary 없음, 서비스 DNS는 global DNS와 다름 `dns=inactive`). 연결 중 사용자가 `route -n get default`에서 interface `ppp0`, `https://www.apple.com` HTTP 200을 확인했습니다. 수신 5773·송신 7513 frame 뒤 Ctrl+C로 `up-result=stopped`: pppd exit 5, TERM 1·KILL 0, `pppd-leftover=ipv6`(공개 pin의 IPv6 더미 잔존과 일치)을 회수 직후 제거, 잔여 0, primary 복귀 yes, SC global IPv4/IPv6/DNS 전후 동일. 모뎀은 LCP Echo-Request에 응답했습니다(`ppp-lcp-echo-reply=1/0`).
- 남은 점: 첫 번째 실행의 쓰기 전 자식 검사 실패 원인, `dns=inactive`의 의미(실제 이름 해석 경로), 원격 LCP·PDP 종료와 후손 프로세스는 미확인입니다.

## LTE 단발 실행기: 구현과 오프라인 검증만

`build/ppp_lte_once`는 기존 고정 launch/session/runner/IF2 USB 구현을 연결합니다. 생산 앱 `main`과 probe enum/source-pointer 정책에는 실행 모드를 추가하지 않습니다. `make build/ppp_lte_once`는 컴파일만 합니다. 자동 검사는 native 실행기의 `--help`와 잘못된 인자만 호출하고 실제 권한·host metadata·SystemConfiguration·process·IP·route·socket·USB 경계는 fake로 치환합니다. 2026-10-17 최종 `make verify-offline`(r7, 일반·ASan/UBSan) 결과는 둘 다 exit 0, 실패 0입니다. offline_stream 1,543,644회, lte-network 10,930회, lte-once 1,627회, 실제 once+network 결합 lte-once-integration 1,575회, auth trial 18,917회 검사이고, ppp_usb도 실패 0입니다(ppp_usb 검사 횟수는 child fixture 타이밍에 따라 실행마다 조금 다릅니다). 실제 session/runner/USB 결합은 기존 `ppp_usb_test` 계약 검사가 맡고, once 테스트에서는 fake입니다. 이 결과는 실제 USB·pppd·LTE 동작 증거가 아닙니다.

echo 상대는 사용자가 통제하는 서버여야 합니다. 클라이언트가 `SHUT_WR`로 half-close한 뒤에도 받은 바이트를 모두 돌려주고 연결을 닫아야 합니다. `ncat --exec /bin/cat`은 half-close를 받으면 응답 전에 닫혀 부적합했습니다. HTTPS 프록시 뒤 서버는 raw TCP echo가 안 됩니다. 통신사 투명 프록시 가능성 때문에 80보다 443 같은 포트를 씁니다. 상대 서버는 시험할 때만 시간 제한을 걸어 띄웁니다. 2026-10-17 사용자 요청으로 상대 서버를 띄운 뒤, 개발 맥의 일반 TCP 클라이언트로 유선 회선에서 14바이트 정확 echo와 EOF를 확인했습니다. 이 확인은 echo 서버를 일반 TCP로 확인한 범위이며 `ppp_lte_once`·USB·pppd·LTE를 쓰지 않았습니다. LTE 경로 실행은 아래 첫 실기 1회 결과 절에 따로 적습니다.

CLI는 `--lte-once --endpoint <수치 IPv4> --port <1..65535>` 순서만 받습니다. 기본 목적지·DNS·proxy·redirect·재연결·임의 payload 옵션은 없습니다. 누락·중복·알 수 없는 인자·잘못된 주소/포트는 host 접근 전 exit 2입니다. unspecified·loopback·link-local·multicast·limited broadcast와 실행 중 귀속 local-self 목적지는 차단합니다. private unicast 자체를 차단하지 않지만 실제 목적지와 과금 위험은 별도 승인 대상입니다. 이 설명은 실제 실행 명령이나 서버 구축 승인이 아닙니다.

시험 규약은 고정 ASCII `dji-lte-once\r\n` 전부를 한 번 송신하고 `SHUT_WR`한 뒤, 정확히 같은 바이트와 EOF를 받는 것뿐입니다. 길이는 C literal의 `sizeof - 1`로 계산합니다. prefix·잘림·추가 바이트·EOF 없는 timeout은 성공이 아닙니다. 앱 payload는 고정 14B이고 route 응답 버퍼는 512B입니다. 이 값은 PPP/TCP 제어·재전송·과금량의 상한이 아닙니다.

첫 runner start slice 직후 다음 poll 전에 child 시작 식별자를 확보합니다. 직접 소유한 살아 있는 child의 유일한 새 PPP service/IF/index와 point-to-point IPv4 local/peer를 매번 확인합니다. 과거 귀속 성공만으로 송신하지 않습니다. socket 단계마다 bounded read-only scoped `RTM_GET` 응답을 검사하고 `IP_BOUND_IF` set/get index를 일치시킵니다. route 부재·다른 IF·관측 오류는 송신 없이 실패합니다. 기본/host route를 추가하거나 다른 경로로 우회하지 않습니다. 이 검사는 전체 라우팅 테이블 불변이나 절대적인 no-leak 보장이 아닙니다.

child 시작 뒤 전체 목표 60초, IP 준비 최대 30초, TCP 시험 최대 10초입니다. 시작 5초·CONNECT 10초·DRAIN 3초의 기존 budget을 사용합니다. clock 실패/역행은 중단합니다. 동기 SDK/시스템 호출과 callback WAIT 때문에 절대 wallclock 종료는 보장하지 않습니다. 각 비차단 network 단계 사이 runner가 USB 처리를 계속합니다.

앱 응답 뒤에도 원격 LCP·AT·PDP 종료 검증은 없습니다. `peer_done`을 합성하지 않고 socket close 후 stop→TERM→DRAIN deadline→오류 정리/소유 직접 child KILL→reap/EOF/callback quiet→CLOSED를 관측합니다. DRAIN timeout은 예기된 부분 결과이며 앱 echo 성공과 전체 정리 성공은 다릅니다. callback WAIT는 owner를 유지해 계속 pump하고 CLOSED 뒤 handler를 복원합니다. 미확인 원격 종료·후손·PDP를 성공으로 올리지 않아 전체 결과는 exit 3입니다. IP/port/route/요청·응답 원문·개인 경로·인증값은 출력하지 않습니다. SC 비교는 dynamic-store global IPv4/IPv6/DNS와 귀속 service/IF 제거 범위뿐입니다. 설치·복구·네트워크 설정 변경은 구현하지 않습니다.

### 2026-10-17 첫 실기 1회 결과

사용자가 승인 범위의 명령을 직접 한 번 실행했습니다(커밋 `7e50263`, 동글 연결, endpoint는 사용자 통제 echo 서버 443). 결과는 `app-echo=unproven reason=child-or-clock-unproven payload-sent=0 payload-received=0 local=closed session-error=supervisor term-count=0 kill-count=1 attribution=unproven`, exit 3이었습니다. 관측한 것은 앱 payload 송수신 0/0, `session-error=supervisor`(timeout 표기가 아닌 supervisor 오류), PPP 인터페이스 귀속 `unproven`입니다. 정리는 `child=reaped`, stdout/stderr EOF, callback quiet, handler 복원, SC global IPv4/IPv6/DNS 불변이었고, 실행 뒤 읽기 전용 확인에서 pppd 프로세스와 ppp 인터페이스는 보이지 않았습니다. 후손 정리는 미확인입니다. 앱 payload 밖의 USB·PPP·LTE 수준 송수신, 원격 PDP 활성화 여부와 원격 PPP 제어 교환은 이 출력으로 확인하지 못했습니다.

그 실행의 출력은 supervisor 오류 종류를 구분하지 않아 원인을 확정하지 못했습니다. 모뎀 echo는 후보 가운데 하나일 뿐이고, 원인으로 확정하거나 다른 후보보다 앞세울 근거는 없습니다. 이전 `--probe-at` 실측에서 이 모뎀은 `AT\r` 뒤에 `\r\nOK\r\n`을 돌려주는 echo 상태였고, 당시 core는 bare CR 뒤 CR을 CONTROL 오류로 끊었습니다. 그래서 고정 hook의 echo 한 번과 `CONNECT <속도>`를 허용하도록 고쳤습니다. echo 한 번은 `AT+CGDATA="PPP",1` 뒤가 bare CR로 끝나는 echo만 뜻합니다. CRLF로 끝나는 printable 줄은 같은 문구라도 기존처럼 전이·실패 근거가 아닌 무시 줄이며 이 한 번에 세지 않습니다. 이것은 추정에 따른 수정이며 실기로 확인하지 않았습니다.

다음 실행부터는 출력에 고정 label·숫자 진단을 더합니다. `supervisor-error`, `phase-reached`, `error-ms`, `connect-sent`, `output-observed`, `stream`, `stream-error`, `modem-echo`, `modem-result`(`cme:<번호>` 등, 문구 없음), `control-bytes`, `child-output-bytes`, `usb-input-bytes`, `child-stderr-bytes`, `child-exit`(`code:N`/`signal:N`), `exec`입니다. 모뎀 문구·PPP 바이트·식별값은 출력하지 않습니다.

- `phase-reached`는 slice 경계에서 본 오류 전 최고 phase입니다. 한 slice 안에서 지나간 전이는 놓칠 수 있습니다.
- `error-ms`는 supervisor 오류를 처음 관측한 slice 경계에서 따로 읽은 monotonic 시각과 child 시작 시각의 차입니다. 이 조회는 stop·budget·clock 판정에 쓰지 않습니다. 시작 시각이 없거나 조회가 실패·역행하면 `unproven`입니다. `none`은 마지막 관측이 성공했고 그 관측에서도 supervisor 오류가 없을 때만 씁니다.
- 관측이 없거나(discovery·session·runner 거절 등) 마지막 관측이 실패하면 새 진단은 `none`/`no`/`0` 대신 `unproven`입니다. 다만 앞선 유효 관측에서 supervisor 오류를 이미 봤다면 `phase-reached`와 `error-ms`는 그 관측값을 유지합니다. 기존 payload·exit·정리 출력은 바뀌지 않습니다.
- byte count는 child stdout과 완료된 USB read에서 읽은 바이트 수입니다. 큐·stream 수락 여부와 무관합니다.
- `exec=ended`는 exec 상태 pipe가 오류 보고 없이 닫혔다는 뜻입니다. pre-exec 신호로도 생길 수 있어 pppd 실행 성공의 증거가 아닙니다.

위 2026-10-17 최종 r7 `verify-offline` 수치(일반·sanitize exit 0, 실패 0)는 이 진단 변경을 포함한 결과입니다. 오프라인 fake 검사일 뿐 이 수정본의 실기 결과는 없습니다. 재실행에는 새 승인이 필요합니다.

### 2026-10-17 두 번째 실기 1회 결과

사용자가 커밋 `6170c1b`의 실행 파일로 한 번 실행했습니다. 출력은 payload 송수신 `0/0`, `supervisor-error=core`, `phase-reached=connect`, `error-ms=28`, `connect-sent=yes`, `modem-echo=none`, `modem-result=no-carrier`, `control-bytes=14`, `child-output-bytes=46`, `usb-input-bytes=14`였습니다. 정리는 `local=closed`, reap·EOF·callback quiet, SC global 불변이었고, 실행 뒤 pppd 프로세스와 ppp 인터페이스는 보이지 않았습니다.

- `connect-sent=yes`는 고정 `AT+CGDATA` 송신을 시도했다는 뜻입니다. 모뎀이 받은 양의 증거가 아닙니다.
- `modem-result=no-carrier`는 송신 직후 받은 줄의 분류일 뿐입니다. 그 줄이 현재 `AT+CGDATA` 요청의 응답인지는 확정하지 않았습니다. 앞선 명령의 잔여 응답이 큐에 남았을 수 있습니다.
- 상태 조회는 `AT+CGDATA` 실행 뒤 사용자의 「상태 조회 승인」을 받아 개발자가 `--probe-status` 1회와 `--probe-pdp` 1회를 따로 실행한 결과입니다. 그 범위에서 CPIN이 읽기 4회에 결과 `none`이었고, 나머지 명령의 응답 prefix는 `unsolicited`로만 보였습니다. PDP 조회는 `contexts=0`이었습니다. 응답 밀림 가능성이 있어 SIM·망·APN 상태는 확정하지 않았습니다.

이 결과를 계기로 상태·PDP·지원 조회에 응답 귀속 판정(아래 「응답 귀속 판정」)을 더했습니다. 오프라인 수정이며 USB 송수신 정책은 바꾸지 않았습니다. 추가 실기는 없습니다. 이어서 수정 도구로 상태·PDP를 각 1회 조회했더니 모든 명령이 `mismatch`(다른 명령의 에코)였습니다. 그래서 송신 후 자기 에코를 기준으로 응답을 맞추고, 미확인이면 다음 명령을 보내지 않도록 수신 순서를 고쳤습니다(아래 「상태 조회」). 송신 전 IN 읽기는 SDK의 timeout 뒤 data toggle 경고와 기존 재읽기 금지 정책 때문에 넣지 않았습니다. 두 변경 모두 독립 검토 APPROVE입니다. 최종 오프라인 `make verify-offline`(r11)은 exit 0이며, 일반·ASan/UBSan에서 상태 조회 테스트 검사 항목 1,656개, PDP 조회 테스트 854개, 데이터 지원 조회 테스트 2,261개와 기존 회귀 검사가 모두 실패 0이었습니다. 이 결과는 fake 전송과 자체 fixture 검사이며, 수신 순서를 고친 조회 도구는 실제 장치에서 실행하지 않았습니다.

### 2026-10-17 세 번째 실기 1회 결과

사용자가 한 번 실행했습니다. 이번에는 CONNECT 뒤 PPP 데이터 단계까지 갔습니다. 출력은 payload 송수신 `0/0`, `supervisor-error=usb`, `phase-reached=session`, `error-ms=1287`, `stream-error=io`, `modem-echo=seen`, `control-bytes=39`, `child-output-bytes=160`, `usb-input-bytes=260`이었습니다.

- 어떤 USB 오류였는지는 출력에 없어 확정하지 못했습니다. 후보는 IN timeout, 0바이트 완료, OUT 실패입니다. 사용자가 붙여 준 터미널 출력에는 OUT 실패 때 나오는 `WritePipeTO` stderr 줄이 없었으므로 OUT 실패 가능성은 낮지만 배제하지는 않았습니다.
- 당시 IN `ReadPipeAsyncTO`의 no-data/completion timeout은 OUT과 같은 1000ms였습니다. PPP 데이터 단계에서는 1초 동안 IN이 조용할 수 있는데, 이것이 fatal 오류가 됐을 가능성이 있습니다.
- 그래서 사용자 승인 범위에서 두 가지를 오프라인으로 고쳤습니다. IN timeout을 OUT과 분리해 단계 예산 합에 맞췄고(아래 「USB IN timeout 정책」), 첫 USB 오류를 고정 라벨로 출력합니다(`usb-error`, `usb-error-site`).
- 독립 검토는 문서 보완 뒤 승인 범위입니다. 2026-10-17 `make verify-offline`(r14)은 일반·ASan/UBSan 모두 exit 0, 실패 0이었습니다(lte-once 1,745, integration 1,575, ppp_usb 실패 0). fake SDK와 자체 fixture 검사이며, 이 수정은 실제 장치에서 확인하지 않았습니다. 다음 실행 전에는 동글을 다시 연결해야 합니다. timeout 뒤 IN data toggle이 맞지 않을 수 있기 때문입니다.

### 2026-10-17 네 번째 실기 1회 결과

커밋 `6117274`의 실행 파일로 한 번 실행한 결과입니다. 출력은 payload 송수신 `0/0`, `usb-error=none`, `reason=interface-unproven`, `supervisor-error=timeout`, `phase-reached=drain`, `error-ms=33012`, TERM 1회·KILL 0회, `child-exit=code:5`, `usb-input-bytes=422`, `child-output-bytes=846`이었습니다.

- USB 오류 없이 세션 예산이 끝나 DRAIN으로 넘어갔습니다. IN timeout 분리 뒤 USB 경로는 끊기지 않았다는 뜻이지만, PPP 협상이 어디까지 갔는지는 출력에 없었습니다.
- `code:5`는 공개 `pppd.h`의 `EXIT_USER_REQUEST`(5)와 같은 값입니다. 우리가 보낸 SIGTERM 정지로 설명할 수 있어 협상 실패 원인으로 보지 않습니다.
- Apple 공개 pppd(pin `06a73c4`)의 `sys-MacOSX.c` `commit_publish_dict()`는 `phase == PHASE_RUNNING`이고 `protocols_ready`일 때만 SC에 게시합니다. `PHASE_RUNNING`은 `auth.c`의 `np_up()`에서 시작합니다. 그래서 SC 미게시는 IPCP가 Opened에 이르지 못했을 가능성을 가리킵니다. 다만 LCP·인증·IPCP 중 어디서 멈췄는지는 정하지 못했고, 이 pin과 설치본 1029가 같은 동작인지도 확인하지 않았습니다.
- 그래서 다음 실기에서 멈춘 단계를 볼 수 있도록 아래 「PPP 협상 단계 진단」을 오프라인으로 더했습니다. 실제 장치에서는 아직 확인하지 않았습니다.

### 2026-10-17 다섯 번째 실기 1회 결과

커밋 `ac2864a`의 실행 파일로, 동글을 다시 연결한 뒤 사용자가 한 번 실행했습니다. 출력은 `reason=interface-unproven`, payload `0/0`, `usb-error=none`, `supervisor-error=timeout`, `phase-reached=drain`, `error-ms=33026`, TERM 1회·KILL 0회, `child-exit=code:5`, `ppp-lcp=opened`, `ppp-auth=none`, `ppp-ipcp=opened`, `ppp-ipv6cp=none`, `ppp-terminate=none`, `ppp-frames-rx=16`, `ppp-frames-tx=19`, `ppp-fcs-errors=0`이었습니다. 실행 뒤 pppd 프로세스와 ppp 인터페이스는 없었습니다.

- 모뎀은 인증을 요구하지 않았고 LCP와 IPCP는 양방향 Configure-Ack까지 갔습니다. 그런데도 observer가 이 pppd의 PPP 서비스를 귀속하지 못해 앱 송신 전에 멈췄습니다. 당시에는 이를 SC 미게시로 해석했지만, 아래 「여섯 번째 실기와 SC 귀속 원인」에서 게시는 시도됐고 귀속 조건이 맞지 않았을 가능성이 크다는 것을 확인했습니다. 아래 미게시 분석과 `noacsp` 시도는 그 잘못된 전제에서 나왔습니다.
- 공개 pin `06a73c4`에서 SC 게시를 막는 조건:
  - `commit_publish_dict()`는 `phase == PHASE_RUNNING`과 `protocols_ready`가 필요하고, `(acsp_use_dhcp || acscp_protent.enabled_flag) && !acspdhcp_ready`이면 게시하지 않습니다.
  - `check_protocols_ready()`(`auth.c`)는 켜진 network protocol이 모두 STOPPED나 OPENED여야 `protocols_ready`를 켭니다. Apple 전용 ACSCP(`acscp.c`)는 기본으로 켜져 있고(`enabled_flag` 1), `noacsp` 옵션으로 끕니다.
- 다만 pin 코드대로라면 이 조건만으로는 미게시를 설명하지 못합니다. 독립 검토에서 확인한 내용입니다.
  - ACSP 플러그인의 `acsp_plugin_ip_up`이 IPCP up 때 `acspdhcpready_notifier`를 알리므로 ACSP/DHCP 조건은 그때 풀립니다.
  - ACSCP가 늦추는 것은 `protocols_ready`뿐입니다. 이것도 모뎀의 Protocol-Reject나 `acscp_retransmit`의 예비 알림(재전송 두 번 뒤, 몇 초)으로 풀립니다.
  - 그러므로 30초 안에 게시됐어야 합니다. 남은 후보는 설치본 1029와 pin의 차이, 그리고 `ipcp_up`이 `np_up` 전에 끝나는 경로(`sifaddr`·`sifup`·`sifnpafmode` 실패)입니다.
- 사용자 승인을 받아 고정 argv 끝에 `noacsp` 하나만 더했습니다. 원인을 확정한 수정이 아니라, 두 대기 조건(`check_protocols_ready`, `commit_publish_dict`)에서 ACSCP를 빼는 시도입니다. 설치본 `/usr/sbin/pppd`에 `noacsp` 옵션 문자열이 있다는 것만 확인했습니다. 인증 거절·기본 경로·IPv6·로그·env·cwd 정책은 그대로입니다. 이 변경은 실제 장치에서 아직 확인하지 않았습니다.
- 진단 분류기는 ACSCP(0x8235) frame을 `기타`로 세므로, 이번 rx16/tx19에 ACSCP 요청이나 Protocol-Reject가 있었는지는 모릅니다.

### 여섯 번째 실기와 SC 귀속 원인

- 여섯 번째 실기도 다섯 번째처럼 `ppp-lcp=opened`, `ppp-auth=none`, `ppp-ipcp=opened`인데 `reason=interface-unproven`으로 끝났습니다.
- 사용자 승인 아래 두 실행의 pppd 시스템 로그에서 고정 문구 `Committed PPP store`가 나온 횟수만 셌습니다. 두 실행 모두 한 번 이상 있었습니다. 로그 원문은 열람하지 않았습니다. 공개 pin에서는 이 문구가 `SCDynamicStoreSetMultiple()` 호출 직전에 찍히고 호출부는 그 반환값을 보지 않으므로, 횟수는 게시 시도의 증거일 뿐 성공의 증거는 아닙니다. 그래서 게시는 시도됐고 귀속에서 실패했을 가능성이 크다고 봅니다.
- 원인(공개 pin `06a73c4` `sys-MacOSX.c` 기준 추론): `sys_init()`은 `nodetach`일 때(802행, 고정 argv에 있음) 803행에서 `publish_dictnumentry(kSCEntNetPPP, CFSTR("pid"), getpid())`를 부르는데, 이 값을 담을 `publish_dict`는 815행에서야 만듭니다. `update_publish_dict()`는 `publish_dict`가 NULL이면 저장하지 않습니다. 그래서 게시된 `State:/Network/Service/<uuid>/PPP` 항목에 `pid`가 없습니다. 기존 observer는 `pid` 일치를 필수로 요구해 귀속이 성립할 수 없었습니다. detach하는 pppd는 나중에 `sys_reinit()`(3824행)에서 `pid`를 게시할 수 있으므로, 귀속한 key에 나중에 다른 `pid`가 붙으면 그때 모호로 처리합니다.
- `noacsp`는 원인이 아니었습니다. `noacsp` 없이 실행한 다섯 번째에도 `Committed PPP store`가 있었습니다. 고정 argv(15개)에는 그대로 남겨 둡니다.
- 새 귀속 규칙(`src/ppp_trial_observer.c`). 아래를 모두 만족할 때만 귀속합니다.
  1. 직접 child가 살아 있고 프로세스 시작 식별자가 처음 확보한 값과 같습니다.
  2. 시작 전 snapshot이 있고, 거기에 없던 PPP 서비스 key가 지금 정확히 하나입니다. 이 개수에는 `pid`가 없거나 다른 key도 모두 들어갑니다. 둘 이상이거나, 이미 귀속한 key와 다른 새 key가 보이면 `ambiguous`로 송신하지 않습니다.
  3. 그 서비스의 `pid`가 없으면 통과하고, 숫자이고 우리 child PID와 같아도 통과합니다. 다른 값이거나 숫자가 아니면 다른 pppd의 것으로 보고 귀속하지 않습니다.
  4. 시작 전부터 있던 key는 후보로 세지 않습니다. 다만 그 key의 현재 `pid`가 우리 PID와 같으면 PID 재사용 위험으로 보고 `ambiguous`입니다.
  5. `InterfaceName`이 문자열이고, 현재 서비스 중 그 이름이 하나뿐이며, 시작 전 서비스에는 그 이름이 없고, `if_nametoindex()`로 커널에 있어야 합니다.
- 한계: 우리 pppd와 같은 때 다른 PPP 서비스가 새로 뜨면 모호로 멈춥니다. 대신 다른 pppd의 서비스를 잘못 귀속하지 않도록 했습니다. `pid` 없는 새 서비스가 우리 것이라는 근거는 "시작 전 없었고 유일하다"는 시간 범위뿐입니다. 이 pin과 설치본 1029의 동일성은 확인하지 않았습니다. 독립 검토는 APPROVE였고, 2026-10-17 `make verify-offline`(r19)은 일반·ASan/UBSan 모두 exit 0, 실패 0이었습니다(ppp_trial_observer 868, auth trial 18,950, lte-once 1,745). fake 테스트이며 실제 장치에서는 아직 확인하지 않았습니다.
- 네 번째 실기의 SC 미게시 해석(위)도 같은 귀속 실패를 미게시로 읽었을 수 있습니다. 네 번째 실행의 로그는 확인하지 않았습니다.

### 2026-10-17 일곱 번째 실기 1회 결과

커밋 `c4d8086`의 실행 파일로 한 번 실행했습니다. 처음으로 `attribution=unique-observed`, 즉 귀속에 성공했습니다. 출력은 `reason=address-missing`, `error-ms=4409`, payload `0/0`, TERM 1회·KILL 0회, `supervisor-error=core`, `ppp-lcp=opened`, `ppp-ipcp=opened`, `ppp-terminate=none`, `ppp-frames-rx=13`, `ppp-frames-tx=13`, `child-exit=code:5`였습니다.

- `address-missing`이 나오는 조건(`src/ppp_lte_network.c` `addresses()`): 귀속 인터페이스의 IPv4가 0개이고 이미 `pinned`일 때뿐입니다. 아직 pin 전이면 0개는 대기(1 반환)이고, 형식이 틀린 IPv4는 `found=2`라 `address-ambiguous`입니다. 따라서 처음에는 귀속 인터페이스에 point-to-point IPv4가 정확히 하나 있어 pin됐고, 그 뒤 같은 인터페이스의 IPv4가 사라졌습니다.
- 공개 pin의 `ipcp.c` `ipcp_down()`은 demand가 아닐 때(고정 argv에 `demand` 없음) 2027행 `sifdown()`과 2028행 `ipcp_clear_addrs()`를 부르고, `ipcp_clear_addrs()`는 2062행 `cifaddr()`로 주소를 지웁니다. 주소 소실과 맞는 경로이지만, 이번 실행에서 무엇이 IPCP를 내렸는지(Terminate, 재협상 Configure-Request, LCP 재시작 등)는 출력만으로 모릅니다. 원인은 확정하지 않았습니다. 설치본 1029와 pin의 동일성도 미확인입니다.
- `error-ms`는 supervisor 첫 오류를 본 시각입니다. `address-missing` 자체의 시각이나 pin 시각은 출력에 없었습니다. `supervisor-error=core`가 once가 network 실패로 정지한 뒤 정리 과정에서 생긴 결과인지도 확정하지 않았습니다.
- 다음 실기에서 이 둘을 가르려고 요약 줄에 아래 진단을 더했습니다. 고정 label과 숫자만 출력하고 주소·원문·식별값은 넣지 않습니다. 송신·정지·정리·timeout·주소 판정에는 쓰지 않습니다. 기존 `ppp-long-frames` 뒤, observer 진단 앞에 이 순서로 붙습니다.
  - `ppp-ipcp-req`, `ppp-ipcp-ack`, `ppp-ipcp-term=<상대>/<로컬>`: IPCP Configure-Request(code 1), Configure-Ack(code 2), Terminate-Request(code 5) frame 수입니다.
  - `ppp-lcp-req`, `ppp-lcp-term`, `ppp-lcp-echo`, `ppp-lcp-protrej=<상대>/<로컬>`: LCP Configure-Request(1), Terminate-Request(5), Echo-Request(9), Protocol-Reject(8) frame 수입니다.
  - `ppp-data=<상대>/<로컬>`: IPv4 데이터 frame 수입니다.
  - 위 8개는 기존 협상 토큰과 같이 최종 관측이 유효할 때만 숫자이고, 아니면 `unproven`입니다. Terminate-Request가 0이 아니면 그쪽이 종료를 요청한 것입니다. Configure-Request 수에는 재전송도 들어가므로 재협상과 바로 구분되지는 않습니다. 개수만으로 순서나 시각은 알 수 없습니다.
  - `ip-ready-ms=<n|none|unproven>`: 네트워크 단계가 주소를 처음 pin한 시각에서 child 시작 시각을 뺀 값입니다. 네트워크를 시작하지 않았으면 `unproven`, pin 전이면 `none`, 시각이 0이거나 시작보다 앞서면 `unproven`입니다.
  - `network-stage=<ip|open|connect|connected|send|shutdown|receive|done|failed|none>`: 네트워크가 실패했거나 다른 정지 사유로 abort됐으면 처음 `fail()`이 불린 때의 단계(뒤의 abort가 덮어쓰지 않음), 아니면 현재 단계입니다. 네트워크를 시작하지 않았으면 `none`입니다. 주소 확인은 단계와 상관없이 매 step마다 하므로, `address-missing`이면 pin 뒤 주소가 사라진 그 시점의 단계(`ip`~`send` 중 하나)가 나옵니다.
- `ip-ready-ms`와 `error-ms`를 비교하면 pin 뒤 얼마 만에 supervisor 오류가 났는지 볼 수 있습니다. 독립 검토는 APPROVE였고, 2026-10-17 `make verify-offline`(r20)은 일반·ASan/UBSan 모두 exit 0, 실패 0이었습니다(lte-network 11,096, lte-once 1,833, integration 1,588). 검토 뒤 통합 테스트에 `network-stage=receive` 검사 하나를 더해 다시 돌렸습니다(1,589, 일반·sanitize 실패 0). fake 테스트이며 실제 장치에서는 아직 검증하지 않았습니다.

### 여덟 번째 실기 1회 결과와 순서 진단

위 진단을 넣은 실행 파일로 한 번 실행했습니다. 출력은 `attribution=unique-observed`, `reason=address-missing`, `ip-ready-ms=3242`, `network-stage=connect`, `error-ms=4361`, `ppp-ipcp-req=4/5`, `ppp-ipcp-ack=1/1`, `ppp-ipcp-term=0/0`, `ppp-lcp-req=2/1`, `ppp-lcp-term=0/0`, `ppp-lcp-echo=0/0`, `ppp-lcp-protrej=0/0`, `ppp-data=0/2`, frame rx13·tx13, TERM 1회·KILL 0회였습니다.

- 주소는 pin된 뒤 connect 단계에서 사라졌습니다. 양쪽 모두 Terminate-Request가 0이므로, 어느 쪽도 IPCP·LCP 종료를 요청하지 않았습니다. IPCP Configure-Ack는 양방향 1개씩인데 Configure-Request는 상대 4·로컬 5개입니다. 로컬 IPv4 데이터 frame 2개가 나갔고 상대에게서는 0개였습니다.
- 사용자 승인 아래 본 시스템 로그(주소·이름은 가리고 시각과 사건 종류만 기록): 12.383 pppd가 IPCP up(local/remote 주소, `Committed`)을 남겼습니다. 13.474 SC 변경으로 IPMonitor 목록에서 ppp0의 IPv4가 빠졌습니다. 13.485 pppd가 `Connection terminated`를 남겼습니다. 그 사이에는 pppd 로그가 없었습니다. 로그 시각과 once의 `-ms` 값은 기준 시계가 다르므로 바로 맞대지 않았습니다.
- 재협상 후보(미확정): RFC 1661 상태표에서 Opened 상태가 Configure-Request를 받으면(RCR) `tld`(This-Layer-Down)를 거쳐 다시 협상합니다. pppd에서는 `ipcp_down()` → `sifdown()`·`ipcp_clear_addrs()` → `cifaddr()`로 주소가 지워지는 경로입니다. Terminate 0, Configure-Request가 Ack보다 많음, 그 사이 pppd 로그 없음과 맞습니다. 그러나 Configure-Request 수에는 Ack 전 재전송·Nak 뒤 재요청도 들어가므로, 개수만으로는 Opened 뒤의 Configure-Request가 있었는지 모릅니다. 원인은 확정하지 않았습니다.
- 이를 가르려고 요약 줄에 아래 두 토큰을 더했습니다. 송신·정지·정리·timeout·주소 판정에는 쓰지 않고, 고정 label·code·개수만 냅니다(시각·payload·옵션 값·주소 없음).
  - `ip-lost-ms=<n|none|unproven>`: `network-stage` 바로 뒤입니다. 네트워크의 첫 실패가 `address-missing`(pin 뒤 IPv4 0개) 또는 `address-changed`일 때, 그 주소를 읽은 단계의 마지막 시계 값에서 child 시작 시각을 뺀 값입니다. 그 밖의 결과(성공, 다른 실패, pin 전)는 `none`, 네트워크 미시작이나 시각이 시작보다 앞서면 `unproven`입니다. 주소가 실제로 사라진 시각이 아니라 once가 그것을 처음 알아챈 읽기 시각입니다.
  - `ppp-trace=<사건,...|none|unproven>`: observer 진단 바로 앞, 줄의 마지막 협상 토큰입니다. 분류한 frame의 순서를 `<방향>.<프로토콜>[code][*개수]`로 씁니다. 방향은 `p`(상대)·`l`(로컬), 프로토콜은 `lcp`·`pap`·`chap`·`eap`·`ipcp`·`ip6cp`·`ip`·`ip6`·`oth`입니다. 제어 프로토콜에는 code를 십진수로 붙이고(범위 밖 code는 0), 데이터(`ip`·`ip6`·`oth`)에는 code가 없습니다. 제어 frame은 하나가 사건 하나입니다. 데이터 frame은 제어 사건 사이 구간 안에서 방향·프로토콜별로 하나로 합쳐 2개 이상이면 `l.ip*2`처럼 개수를 붙이며, 그 구간 안 데이터끼리의 순서는 첫 등장 순서만 남습니다. FCS가 틀린 frame은 넣지 않습니다.
  - 처음 48개 사건만 순서대로 남기고, 그 뒤는 모두 끝의 `+N`(버린 frame 수)으로만 셉니다. 남은 사건은 실제 순서의 앞부분 그대로입니다. 총수는 기존 `ppp-ipcp-req` 등에 그대로 있습니다. 최종 관측이 유효하지 않으면 `unproven`, 사건이 없으면 `none`입니다. 출력 버퍼는 최악의 경우에 맞춰 잡아 잘리지 않게 했고, 그래도 잘리면 `~`를 붙입니다.
  - 예: `ppp-trace=p.lcp1,l.lcp1,l.lcp2,p.lcp2,l.ipcp1,p.ipcp2,l.ip*2,p.ip6cp1,p.lcp1` (fake 테스트의 입력입니다. 실기 값이 아닙니다.)
- `ppp-trace`에서 IPCP Ack(`ipcp2`)가 양쪽에 나온 뒤, 어느 방향이든 `lcp1`이나 `ipcp1`이 다시 나오면 위 재협상 후보와 맞습니다. RFC 1661상 Opened 상태에서 LCP Configure-Request를 받아도 LCP가 내려가면서 IPCP가 함께 내려갑니다. 이때 `ipcp5`·`lcp5`(Terminate-Request)가 없으면 종료 요청이 아니라 재협상으로 내려간 것입니다. 양쪽 Ack 뒤 Configure-Request가 없으면 다른 경로입니다. 독립 검토는 APPROVE였고, 2026-10-17 `make verify-offline`(r21)은 일반·ASan/UBSan 모두 exit 0, 실패 0이었습니다(ppp_negotiation 97, lte-network 11,159, lte-once 1,928, integration 1,736). fake·자체 fixture 테스트만 더했고, 실제 장치에서는 아직 검증하지 않았습니다.

### 아홉 번째 실기 1회 결과와 주소 안정 대기

커밋 `6460d96`의 실행 파일로 한 번 실행했습니다. 출력은 `reason=address-missing`, `ip-ready-ms=3323`, `ip-lost-ms=4416`, `network-stage=connect`였습니다. `ppp-trace`는 아래와 같습니다.

`l.lcp1,p.lcp1,p.lcp2,l.lcp4,p.lcp1,l.lcp2,l.ipcp1,p.lcp11,l.ipcp1,p.ipcp1,p.ipcp4,l.ipcp3,l.ipcp1,p.ipcp1,p.ipcp3,l.ipcp2,l.ipcp1,p.ipcp2,l.ip*2,p.ipcp1,p.ipcp4,l.ipcp1,l.ipcp3,p.ipcp4,p.ipcp1`

- `l.ipcp2`·`p.ipcp2`로 양쪽 IPCP Configure-Ack가 나와 IPCP가 Opened가 됐습니다. 바로 뒤 로컬 IPv4 frame 2개(`l.ip*2`)가 나갔습니다. connect 직후라 SYN으로 보지만, frame 내용은 기록하지 않으므로 확정은 아닙니다.
- 그다음 모뎀이 IPCP Configure-Request(`p.ipcp1`)를 다시 보냈습니다. 여덟 번째 실기에서 세운 재협상 후보와 맞습니다. RFC 1661 상태표에서 Opened 상태가 Configure-Request를 받으면 `tld`(This-Layer-Down)를 거쳐 다시 협상하고, pppd에서는 `ipcp_down()` → `cifaddr()`로 주소가 지워집니다. 뒤로도 `ipcp5`·`lcp5`(Terminate-Request)는 없으므로 종료 요청이 아니라 재협상입니다.
- pppd 로그에서는 모뎀의 Configure-Request에 IP-Address 옵션이 없어 pppd가 원격 주소를 기본값으로 대신 썼습니다(`defaulting to`). 그 1.1초 뒤 once가 TERM을 보냈습니다. 모뎀이 왜 재협상했는지는 확정하지 않았습니다. once가 바로 정지해서 재협상이 끝나 주소가 다시 붙는지도 보지 못했습니다.
- `ip-lost-ms - ip-ready-ms`는 1093ms입니다. pin 뒤 약 1.1초 만에 once가 주소가 없어진 것을 처음 읽었습니다(실제 소실 시각은 그 이전).

사용자 승인으로 연결 전 주소 안정 대기를 넣었습니다(`src/ppp_lte_network.c`).

- IP 단계(route 확인·TCP socket 전)에서는 pin한 주소가 사라지거나 local/peer 쌍이 바뀌어도 실패하지 않습니다. pin을 풀고 안정 대기를 처음부터 다시 시작합니다. 바뀐 쌍은 다음 읽기에서 새로 pin합니다. 대기 중이던 RTM_GET이 있으면 버리고 route socket도 닫습니다. 다음 안정 판정 뒤 새 소켓·새 sequence로 다시 묻습니다. 소켓을 닫으므로 이전 질의의 응답은 오지 않습니다.
- 같은 쌍이 `PPP_LTE_ADDRESS_SETTLE_MS`(3000ms) 동안 이어진 뒤에야 route socket·RTM_GET·TCP socket으로 넘어갑니다. 그 전에는 route 질의도 하지 않습니다.
- IP 준비 한도(시작 + 30초)와 전체 60초·앱 10초 한도는 그대로입니다. 30초 안에 안정되지 않으면 기존 deadline 경로(`readiness-gate`)로 끝납니다.
- 둘 이상이거나 형식이 틀린 IPv4(`address-ambiguous`)는 IP 단계에서도 바로 실패합니다.
- TCP socket을 연 뒤(`open` 이상)는 기존 규칙 그대로입니다. 사라지면 `address-missing`, 바뀌면 `address-changed`로 바로 실패합니다. connect는 1회뿐이고 재시도하지 않습니다.
- 아홉 번째 실기처럼 pin 1.1초 뒤 재협상이 시작되면, 새 규칙에서는 connect 전 IP 단계에서 대기가 다시 시작됩니다. 재협상이 30초 안에 끝나 주소가 다시 붙고 3초간 유지돼야 연결합니다. 그렇게 되는지는 실기에서 확인하지 않았습니다.

진단 토큰을 `ip-ready-ms` 뒤에 더했습니다. 판정에는 쓰지 않습니다.

- `ip-stable-ms=<n|none|unproven>`: 마지막 pin이 안정 판정을 통과한 시각에서 child 시작 시각을 뺀 값입니다. 아직 통과하지 않았거나 대기가 다시 시작돼 무효가 됐으면 `none`, 네트워크 미시작이나 시각이 시작보다 앞서면 `unproven`입니다.
- `ip-resets=<n|unproven>`: IP 단계에서 pin을 풀고 대기를 다시 시작한 횟수입니다. 포화합니다. 네트워크 미시작이면 `unproven`입니다.
- `ip-ready-ms`는 계속 처음 pin한 시각입니다. 다시 pin해도 덮어쓰지 않습니다. `ip-lost-ms`는 실패로 끝난 소실(`open` 이후)에만 숫자이고, 대기 중 소실은 `ip-resets`로만 셉니다.
- 대기 중 reset이 오면 route socket도 닫습니다. 새 대기 동안 읽지 않는 소켓에 재협상 broadcast가 쌓여 나중의 RTM_GET 응답이 버려지는 일을 막으려는 것입니다. 다시 안정되면 새 소켓으로 질의합니다.
- fake·자체 fixture 테스트(`ppp_lte_network_test`, `ppp_lte_once_test`, `ppp_lte_once_integration_test`)만 더했고, 실제 장치에서는 아직 검증하지 않았습니다. 독립 검토는 APPROVE였고, 2026-10-17 `make verify-offline`(r24)은 일반·ASan/UBSan 모두 exit 0, 실패 0이었습니다(lte-network 29,216, lte-once 1,928, integration 5,204).

### 2026-10-17 열 번째 실기 1회 결과: 첫 LTE 송수신 성공

커밋 `fa745c1`의 실행 파일로, 동글을 다시 연결한 뒤 사용자가 한 번 실행했습니다. 사용자 소유의 인터넷 echo 서버(공인 IPv4, TCP 443)로 보낸 고정 14바이트가 그대로 돌아오고 EOF까지 확인됐습니다.

- 출력: `app-echo=exact-eof reason=echo-observed payload-sent=14 payload-received=14`, `attribution=unique-observed`, `network-stage=done`, `ip-lost-ms=none`, exit 3.
- 주소: `ip-ready-ms=3256`에 처음 pin했고, `ip-resets=1`로 한 번 다시 대기한 뒤 `ip-stable-ms=8279`에 안정 판정이 났습니다. pppd 로그(고정 문구 횟수만 확인, 주소는 가림)에도 IPCP up이 두 번 있었습니다(12.365초, 14.426초).
- `ppp-trace`: 첫 IPCP Opened 뒤 모뎀이 다시 Configure-Request를 보냈습니다. Nak·Reject를 몇 번 주고받은 뒤 두 번째 Opened(양쪽 `ipcp2`)가 됐고, 그 뒤 `l.ip*8,p.ip*6` 데이터 frame이 오갔습니다. 아홉 번째 실기와 같은 재협상이 이번에도 일어났고, 안정 대기가 이를 넘겼습니다.
- 송신 경로: 귀속된 PPP 인터페이스에 `IP_BOUND_IF`로 묶은 소켓, scoped `RTM_GET`으로 확인한 경로입니다. 시스템 기본 경로·DNS·NAT는 바꾸지 않았고 SC global IPv4/IPv6/DNS는 실행 전후가 같았습니다(`sc-global-*=unchanged`).
- 정리: TERM 1회·KILL 0회, `child=reaped`, stdout/stderr EOF, callback quiet, handler 복원, `service-state=removed`, `kernel-interface=removed`였습니다. 실행 뒤 pppd 프로세스와 ppp 인터페이스도 남지 않았습니다.
- 한계: 원격 LCP·PDP 종료는 여전히 확인하지 못해 `overall=partial`(exit 3)입니다. `supervisor-error=timeout`은 `peer_done`을 합성하지 않아 생기는 예상된 DRAIN timeout입니다. 후손 프로세스는 미확인입니다. 이 실행은 단발 시험이며, 꽂으면 연결되는 상시 동작이나 다른 기기와의 인터넷 공유는 아직 구현하지 않았습니다.

### USB IN timeout 정책

- OUT(`WritePipeTO`, 고정 `AT+CGDATA` 포함)은 기존 timeout을 그대로 씁니다. 기본값은 1000ms입니다.
- IN(`ReadPipeAsyncTO`)은 세션이 `connect_ms + session_ms + drain_ms + 1000`ms로 계산해 따로 넘깁니다. 이 값이 0이거나 uint32_t를 넘으면 세션 시작을 거절합니다. `ppp_lte_once`의 예산이면 74000ms입니다.
- IN timeout 완료는 여전히 fatal입니다. 재제출, stall 해제, reset, 0 timeout은 하지 않습니다. timeout 뒤 data toggle 경고와 재읽기 금지 정책도 그대로입니다.
- 우리가 요청한 취소 중에 오는 `kIOReturnAborted`와 `kIOUSBTransactionReturned`만 정상 종료로 봅니다. 취소 중이 아닐 때의 aborted는 오류입니다.
- `ppp_lte_once`는 첫 USB 오류를 `usb-error=<timeout|aborted|no-device|not-open|stall|overrun|zero-length|oversize|other|none>`와 `usb-error-site=<submit|completion|write|connect|abort|validate|none>`로 출력합니다. 이 요약 줄(stdout)에는 raw IOReturn 값을 넣지 않습니다. 다만 기존 `usb_observer`는 OUT 쓰기 timeout·실패와 interface close 실패 때 stderr에 `IOReturn=0x…`를 계속 출력합니다. 최종 관측이 유효하지 않으면 `unproven`입니다. 어댑터가 supervisor에 바인딩되기 전의 오류는 라벨에 나오지 않습니다.
- 라벨은 원인을 대략 나누는 진단입니다. 송신 전 장치·pipe 재확인 실패는 이유와 관계없이 `no-device`(위치 `validate`)이고, OUT 쓰기의 점유·미개방 실패는 `other`입니다. 호출자 버퍼보다 큰 완료는 위치가 `completion`으로 붙습니다.

### PPP 협상 단계 진단

`src/ppp_negotiation.h/c`는 relay가 이미 받아들인 바이트의 사본만 RFC 1662 frame으로 나눠 셉니다. relay는 여전히 PPP를 해석하거나 바꾸지 않습니다. 진단 상태는 송신·정지·정리 판단에 쓰지 않고, 응답·주입·차단도 하지 않습니다.

- 연결 지점은 supervisor 두 곳입니다. `read_usb`는 USB 입력 중 core가 RX 큐에 받아들인 끝부분만, `flush_pending`은 pppd 출력 중 TX 큐에 받아들인 끝부분만 넘깁니다. CONNECT 전 제어 문자열(`AT+CGDATA` 에코, `CONNECT` 줄)은 큐에 들어가지 않으므로 보지 않습니다.
- `ppp_lte_once` 요약 줄에 아래 토큰을 더했습니다. 최종 관측이 유효하지 않으면 모두 `unproven`입니다.
  - `ppp-lcp=<none|configuring|opened>`
  - `ppp-auth=<unknown|none|pap|chap|eap|other>`: 상대가 보낸 마지막 LCP Configure-Request의 option 3 프로토콜 번호만 읽습니다.
  - `ppp-auth-result=<none|pending|success|failure>`: 상대가 보낸 PAP Ack/Nak, CHAP·EAP Success/Failure 코드만 봅니다.
  - `ppp-ipcp`, `ppp-ipv6cp=<none|configuring|nak|rejected|opened>`
  - `ppp-terminate=<none|local|peer|both>`: LCP Terminate-Request를 보낸 쪽입니다.
  - `ppp-frames-rx`, `ppp-frames-tx`, `ppp-fcs-errors`, `ppp-long-frames`: 방향별 유효 frame 수와 양방향 합계입니다. `ppp-fcs-errors`에는 FCS 불일치뿐 아니라 abort(`7d 7e`)와 너무 짧은 frame도 들어갑니다. `ppp-long-frames`는 4096바이트를 넘은 frame입니다.
  - `opened`·`nak`·`rejected`는 모두 "한 번이라도 보였다"는 누적 뜻입니다. 지금 상태를 뜻하지 않습니다. 예를 들어 `ppp-ipcp=nak`는 상대가 Nak를 한 번 이상 보냈다는 뜻이며, 그 뒤 협상이 진행됐는지는 이 라벨만으로 알 수 없습니다.
  - 상대가 LCP Protocol-Reject로 IPCP·IPv6CP를 거부해도 해당 라벨에는 반영되지 않습니다(`configuring`으로 남습니다).
- `opened`는 양방향 Configure-Ack가 한 번이라도 보였다는 뜻입니다. 지금도 열려 있다는 뜻은 아닙니다. `nak`·`rejected`는 상대가 보낸 frame만 셉니다.
- frame마다 앞 64바이트만 잠시 들고 분류 직후 0으로 지웁니다. 출력과 관측 구조체에는 고정 크기 개수와 라벨만 남습니다. payload, option 값, IP 주소, DNS, 인증 이름·challenge·response, 원문·16진수는 저장하거나 출력하지 않습니다. 다음 flag 전의 미완성 frame 앞부분은 실행 중 supervisor 메모리에 남을 수 있습니다. 세션을 해제하기 전에 이 앞부분을 0으로 지웁니다.
- ACCM 옵션 값은 읽지 않습니다. 대신 방향마다 초기 ACCM대로 제어 문자를 지운 해석과 그대로 둔 해석을 함께 계산하고, FCS가 맞는 쪽을 씁니다. RFC 1662에 없는 진단용 우회이며, 잘못된 frame이 우연히 FCS를 통과할 확률(약 1/65536)이 있습니다.
- 4번째 실기 근거와 연결한 오프라인 수정입니다. 독립 검토는 APPROVE였고, 2026-10-17 `make verify-offline`(r16)은 일반·ASan/UBSan 모두 exit 0, 실패 0이었습니다(ppp_negotiation 76, lte-once 1,745, integration 1,575, ppp_usb·supervisor 실패 0). fake SDK·합성 frame·자체 fixture 검사이며 실제 장치에서는 아직 확인하지 않았습니다.

## 동글 없는 설치 PPP 인증 단발 시험

생산 앱과 분리한 `build/ppp_auth_trial --local-auth-once`를 추가했습니다. 이 단계는 설치된 `/usr/sbin/pppd` 직접 자식 하나와 같은 실행기 안의 로컬 pipe 상대만 사용합니다. USB·SIM·LTE·IP 송수신·공유를 시작하지 않습니다. 기존 `ppp_local_check`의 root 거절과 생산 launch 정책·child ABI·session/runner·USB·main은 그대로입니다.

`make build/ppp_auth_trial`로 빌드합니다. `--help`와 `--local-auth-once` 단독만 받습니다. 도움말·잘못된 인자는 권한·metadata·host 조회나 exec를 하지 않습니다. 일반·ASan/UBSan 테스트와 `verify-offline`은 합성 프레임과 fake 경계만 실행합니다. 실행기 실제 실행은 자동 검사에 포함하지 않습니다.

승인된 실제 시험 명령은 개발자가 검증·독립 검토 뒤 아래 그대로 한 번만 호출하는 범위입니다.

```sh
/usr/bin/sudo -N -n <repo>/build/ppp_auth_trial --local-auth-once
```

`-N` 미지원·권한 거절·암호 필요·preflight 차단이면 중단합니다. 옵션 제거, 재시도, `sudo -v`/`sudo -k`, cache 조작, root 소유 위치 복사·helper·setuid·sudoers·launchd 설치는 하지 않습니다. 사용자 소유 빌드를 직접 검토한 뒤 root로 실행하므로 빌드/경로 변조와 검사→exec TOCTOU 위험은 남습니다. root 소유 검사는 설치 `pppd`와 부모, 아래 trial 전용 home 검사에 적용하고 사용자 소유 시험 빌드에 강제하지 않습니다.

실행 시 UID/EUID root 일치·GID/EGID 일치, 설치 실행 파일/부모의 lstat 소유·mode·type과 기존 preflight clear를 요구합니다. 이번 수정은 trial 전용으로 `/`와 실제 root passwd home의 물리 경로 전체에 존재하는 non-symlink 디렉터리·root 소유·group/other 쓰기 금지를 추가합니다. 기존 home 검사는 소유권·쓰기 제한을 보장하지 않았습니다. `/var` 또는 `/var/…` 경계에서만 root 소유 symlink와 정확한 `private/var` 또는 `/private/var` 대상을 확인해 고정 `/private/var`로 매핑합니다. 일반 `realpath()`·임의 alias·`.`/`..`·빈 구성요소·경로 상한 초과는 허용하지 않습니다. 검사한 동일 home을 기존 `ppp_metadata_backend.home`에 전달하므로 `.ppprc`도 그 물리 경로에서 검사하고 고정 시스템 항목은 모두 유지합니다. 공용 NULL 정책·symlink 차단 정책은 바꾸지 않습니다. 설정·인증 파일 내용이나 시스템 로그는 읽지 않습니다. executable 포함 argv 14개·빈 환경·cwd `/`를 변경하지 않습니다. metadata 검사와 pppd의 원래 passwd home 사용 사이 TOCTOU·경로 동일성 한계는 남습니다.

차단 출력은 기존 `gate=root-home-preflight` 아래 `reason`으로 home 조회/형식, alias metadata/type/owner/readlink/target, home metadata/type/owner-mode, 공용 preflight 실패를 구분합니다. `preflight=complete`일 때만 고정 항목별 metadata 상태를 표시합니다. 선행 실패는 `preflight=not-run`, 공용 호출 실패는 `preflight=error`, 결과 상태 불일치는 `preflight=invalid`이며 초기 zero 항목을 absent 관측으로 출력하지 않습니다. 개인 경로·UID·raw errno·링크 원문은 출력하지 않습니다. `auth-reject=unverified child-cleanup=not-started`와 차단 exit 3을 유지합니다.

상대는 exec 성공과 첫 유효 LCP Configure-Request 뒤 PAP, CHAP-MD5, MSCHAP, MSCHAPv2, EAP의 정상 단일 인증 CI를 서로 다른 ID로 각 한 번만 보냅니다. 같은 ID와 옵션의 정확한 복사인 Configure-Reject 다섯 개만 `auth-reject=observed-all`로 인정합니다. Ack/Nak·잘못된 ID/옵션·예상 밖 프로토콜·FCS/길이 오류·상한 초과 뒤에는 새 요청을 보내지 않습니다. **pppd의 Configure-Request에는 답하지 않아 RFC 1661 §5.1 MUST reply를 따르지 않는 불완전한 진단 상대입니다.** LCP Opened·인증·IPCP를 유도하거나 인증값을 제공하지 않습니다.

임시 커널 PPP unit/인터페이스, SystemConfiguration 게시, pid/lock 파일, 시스템 로그가 생길 수 있습니다. 공개 pin에는 PPP 지원 확인 중 내장 구성요소 로드 시도 경로도 있습니다. 설치본과 공개 소스의 일치는 미확인이며 시스템 무영향을 보장하지 않습니다. 기본 경로·DNS·NAT를 의도적으로 바꾸지 않지만 커널 라우팅 테이블 전체를 수집하거나 불변을 주장하지 않습니다.

프로브 목표 20초, TERM 유예 3초, KILL 뒤 관측 2초이며 동기 호출의 절대 시간 보장은 없습니다. stdin/stdout/stderr는 각각 누적 64KiB, slice 공유 예산은 4096바이트입니다. stderr의 다음 poll 최대 4096바이트를 예약하지 못하면 `stderr-reserve-limit`으로 멈추고 status-only로 정리합니다. 실제 상한 도달이나 관측 EOF로 바꾸지 않습니다. handler는 flag만 기록하고 정상 문맥에서 처리/복원합니다. 직접 자식에만 TERM/KILL 각 최대 한 번이며 ECHILD 뒤에는 signal하지 않습니다.

출력은 고정 분류와 개수뿐입니다. raw PPP·인증값·계정/UID·개인 경로·IP/DNS 주소는 출력하지 않습니다. pppd 자체의 기존 사용자·UID·IP/DNS syslog 가능성은 억제하지 않습니다. `auth-reject`, exec 확인, 직접 child reap/ownership loss/unresolved, transport EOF/포기, handler 복원, 귀속/인터페이스 상태와 `descendants=unproven`은 별개입니다. unresolved이면 살아 있는 child를 close 성공으로 꾸미지 않고 비성공으로 반환합니다. 반환/프로세스 종료 때 OS가 fd를 닫고 child가 재부모화될 수 있어 반환 후 소유·자원 유지를 보장하지 않습니다.

SystemConfiguration 관측은 시작 전·실행 중·종료 뒤로 나눕니다. reap 전에 저장한 PID/프로세스 시작 식별자, 시작 전 snapshot에 없던 유일한 PPP 서비스 key와 고유 `InterfaceName`을 결합해 유일한 서비스/인터페이스만 귀속합니다. 공개 pin에서는 `pid`가 게시되지 않으므로 `pid`는 있을 때만 대조합니다(위 「여섯 번째 실기와 SC 귀속 원인」). 새 서비스 여러 개·다른 `pid`·`InterfaceName` 미게시·시작 식별자 실패·중복·놓친 일시 상태는 `attribution=unproven`입니다. 종료 뒤에는 저장한 서비스 key만 조회하며 같은 숫자 PID를 다시 현재 child로 간주하지 않습니다. `sc-global-ipv4/ipv6/dns`는 Dynamic Store global snapshot 비교만 뜻합니다. 실제 잔여/변경은 `next-run=blocked-until-resolved`, 관측 한계는 별도 검토·승인 대상입니다. 추측한 인터페이스 down·route/Store 삭제 같은 복구는 없습니다.

2026-10-16 기존 구현에 대한 개발자의 최종 오프라인 검증은 `build/ppp-auth-trial-20261016-r5/`에 기록됐고 gate는 exit 0입니다. 일반 19개와 ASan/UBSan 19개, 총 38개 suite summary가 모두 `failures=0`입니다. 새 검사는 각각 probe 8,139회·trial 7,058회·observer 396회이며 일반/ASan·UBSan 검사 양쪽에서 통과했습니다. 잘못된 CLI 인자 67건의 status는 모두 exit 2입니다. r1–r5의 증분 빌드에서 SDK와 production 컴파일을 수행했습니다. stderr 3,346줄의 오류 패턴 검색은 일치가 없었으며 전문 독해 결과는 아닙니다.

별도 검토와 검증 뒤 개발자가 위 승인 명령을 한 번 호출했지만 `sudo: a password is required`로 차단됐습니다. `local_auth_exit=1`, stdout은 비어 있으며 결과 파일은 `build/ppp-auth-trial-20261016-r5/local-auth.*`입니다. `reviewed.sha256`의 10개 항목은 호출 전후 모두 OK였습니다. trial과 설치 pppd는 실행되지 않았습니다. root preflight·SystemConfiguration·다섯 인증 방식의 실제 거부·child/transport/인터페이스 정리는 관측하지 못해 미검증입니다. USB·LTE 실행과 재시도·sudo cache 조작은 하지 않았습니다. 이 결과로 후속 LTE 실행을 허용하지 않습니다.

이후 사용자가 직접 실행한 결과는 별도 기록입니다. 사용자 보고는 `execution=blocked gate=root-home-preflight auth-reject=unverified child-cleanup=not-started`, exit 3입니다. 이 실행에서도 설치 pppd는 시작되지 않았습니다. 기존 출력은 home 검사와 공용 preflight 실패를 구분하지 않아 원인은 미확정입니다. `/var` 시스템 링크는 가능한 원인일 뿐 호스트 metadata로 확인하지 않았습니다.

이번 home/alias/진단 수정의 최종 오프라인 gate는 개발자가 `build/ppp-home-preflight-20261016-r2/verify.status`의 `verify_exit=0`으로 확인했습니다. 일반 19개·ASan/UBSan 19개, 총 38개 suite summary가 모두 `failures=0`입니다. trial 18,917회·probe 8,139회·observer 396회 검사가 각각 양쪽에서 통과했습니다. 새 fake 회귀 검사는 상대/절대 고정 alias·직접 물리 경로·다른 alias 거절·owner/mode·경로 경계·매핑된 `.ppprc`와 고정 시스템 항목·not-run 진단·출력 길이/오류를 포함합니다. r1에서 SDK/production 컴파일은 통과했지만 테스트의 `snprintf` 매크로 충돌로 gate가 멈췄고, `#undef snprintf` 최소 수정 뒤 r2가 통과했습니다. r2 stderr 3,346줄의 오류·FAIL·runtime·sanitizer·make 패턴 검색은 일치가 없었으며 전문 독해는 아닙니다. 별도 정적 검토는 `APPROVE`, 발견한 실제 결함 0건입니다. 실제 재시험은 하지 않았습니다. 이번 수정·오프라인 검증 중 host root home/설정 조회·root 실행·sudo·설치 pppd·USB·LTE·시스템 변경은 하지 않았습니다. 오프라인 통과와 정적 검토는 사용자 보고 차단 원인이나 실제 인증/정리를 확인한 결과가 아닙니다.

## 확인한 범위

- 실제 캐시 설명자: VID `2ca3`, PID `4009`, release `0318`, 구성 값 `1`, 전체 `209`바이트, 고유 인터페이스 `5`개, alternate setting은 모두 `0`.
- 인터페이스 1~3에 CDC와 닮은 `0x24` 하위 설명자 0/1/2/6이 있습니다. union master/slave 및 dataInterface 값은 모두 0입니다. 이를 표준 ACM, AT, QMI 지원의 증거로 취급하지 않습니다.
- 실제 fixture는 `/tmp/dji-usb-desc-20261013/output.txt`의 구성 바이트를 그대로 옮겼습니다. 기존 `/tmp/dji-usb-desc-20261013/descriptor.c`의 단일 장치 검색, accessor VID/PID 재확인, 캐시 접근과 정리 패턴을 재사용했습니다. 원본 파일은 변경하지 않았습니다.
- 현재 빌드·실행 검증 환경은 Apple Silicon arm64, macOS 26.6.2, 설치된 Command Line Tools SDK입니다. 다른 macOS 버전이나 Intel Mac은 검증하지 않았습니다. `kIOMainPortDefault`와 legacy IOUSB accessor가 제공되는 SDK가 필요합니다.

## 실장치 관측 (2026-10-13)

macOS 26 arm64에서 VID `2ca3`, PID `4009`, release `0318` 장치를 독립 검토 뒤 한 번 관찰했습니다. 인터페이스 0의 BULK IN `0x81`에서 16바이트를 받은 뒤 timeout이 났습니다. 인터페이스 1~4는 각 관찰 시간 안에 데이터를 받지 못했습니다. 전체는 6회 읽기·5회 timeout·11,189ms였고 종료 코드는 0, stderr는 비어 있었습니다.

이 결과는 해당 실행에서 인터페이스 접근과 BULK IN 읽기가 됐다는 확인만 뜻합니다. 프로토콜은 미확정이고 LTE 접속은 하지 않았습니다. 16바이트의 내용은 출력·저장·분석하지 않았습니다. 재실행 때 같은 응답이 나온다고 보장하지 않습니다.

### 재관측 (2026-10-14)

독립 검증 뒤 coordinator가 `--observe --analyze`를 한 번 실행했습니다. 인터페이스 0~4 모두 timeout이 났고, 전체는 수신 0바이트·5회 읽기·5회 timeout·11,078ms였습니다. 종료 코드는 0이고 stderr는 비어 있었습니다. 분석 결과는 `analysis status=no-data protocol=unconfirmed`입니다.

이전 16바이트는 재수신되지 않아 형식을 확인하지 못했습니다. 이번 관찰 시간 안에 데이터가 없었다는 결과이지 프로토콜 미지원의 증거는 아닙니다. 최초 초기화 시점에만 응답했을 가능성은 가설이며 확인하지 않았습니다. 재연결이나 드론 캡처는 이 단계에서 실행하지 않았습니다.

## 실행 모드

| 명령 | 동작 |
|---|---|
| `./build/dji-lte-mac` | 정확히 하나인 대상 장치를 찾아 IOKit 캐시 설명자만 조회합니다. |
| `./build/dji-lte-mac --fixture tests/fixtures/dji-2ca3-4009-config.hex` | 장치 없이 ASCII hex 파일을 파싱합니다. |
| `./build/dji-lte-mac --help` | 옵션과 접근 경계를 표시합니다. |
| `./build/dji-lte-mac --observe` | 승인된 단일 장치의 점유되지 않은 인터페이스를 non-seize로 열고 BULK IN만 제한해서 읽습니다. 실장치 실행은 독립 검토 뒤 진행합니다. |
| `./build/dji-lte-mac --observe --analyze` | 같은 관찰 상한 안에서 성공한 읽기의 형식 후보와 바이트 통계만 메모리에서 분석합니다. 읽기당 최대 4096바이트이며 원본은 출력·저장하지 않습니다. |
| `./build/dji-lte-mac --probe-at` | 사용자가 승인한 단독 모드입니다. 인터페이스 2에 고정 3바이트 `AT\r`를 한 번 보내고 응답 형식만 분류합니다. 아래 「AT 1회 송신」을 보세요. |
| `./build/dji-lte-mac --probe-ati` | 사용자가 승인한 단독 모드입니다. 인터페이스 2에 고정 4바이트 `ATI\r`를 한 번 보내고 응답 줄을 가림 규칙에 따라 출력합니다. 아래 「ATI 1회 송신」을 보세요. |
| `./build/dji-lte-mac --probe-status` | 사용자가 승인한 단독 모드입니다. 인터페이스 2에 상태 조회 명령 4개를 정해진 순서로 한 번씩 보내고 명령별로 정한 필드만 출력합니다. 아래 「상태 조회」를 보세요. |
| `./build/dji-lte-mac --probe-pdp` | 사용자가 승인한 단독 모드입니다. 인터페이스 2에 `AT+CGDCONT?`를 한 번 보내 데이터 연결 설정을 읽습니다. 아래 「데이터 연결 설정 조회」를 보세요. |
| `./build/dji-lte-mac --probe-data-support` | 사용자가 승인한 단독 모드입니다. 인터페이스 2에 `AT+CGDATA=?`를 한 번 보내 알려진 L2P 지원 목록만 출력합니다. 아래 「데이터 방식 지원 조회」를 보세요. |
| `./build/dji-lte-mac --wait-reconnect --observe --analyze` | 새 registry ID의 단일 대상이 나타나면 기존 분석을 한 번 실행합니다. 이 순서만 허용하며 실장치 실행은 독립 검토 뒤 coordinator가 진행합니다. |

`--open`이나 임의 USB read/write/control 옵션은 없습니다. 기본 실행과 fixture 실행은 `USBDeviceOpen`, `USBInterfaceOpen`, control/bulk 전송, 문자열·serial 읽기를 호출하지 않습니다. IOKit user-client/plugin 객체 생성은 캐시 accessor에 필요하며 USB 장치를 여는 것과 다릅니다. 조회가 끝나면 객체를 모두 해제합니다.

출력에는 인터페이스·endpoint 번호, 방향, 전송 종류, 최대 packet 크기, 설명자 메타데이터만 남깁니다. raw hex와 USB 문자열, IMEI·IMSI 등 개인 식별값은 출력하지 않습니다. 임의/알 수 없는 설명자도 내부 바이트와 offset/type/length 기록에 그대로 보존하지만 raw 출력은 하지 않습니다.

## 수신 형식 분석

`--analyze`는 `--observe` 뒤에 붙이는 옵션입니다. 단독 사용, 순서 바꾸기, fixture/help와 혼용은 장치 검색·열기 전에 종료 코드 2로 거부합니다. 기본 캐시 조회와 `--observe` 출력·읽기 크기는 그대로입니다. 분석 모드만 각 읽기 요청을 최대 4096바이트로 제한합니다. 2초/인터페이스·15초/전체·64KiB/전체·8회/인터페이스 상한은 바꾸지 않습니다. 더 읽거나 timeout 뒤 재시도하지 않습니다.

성공한 읽기 버퍼만 즉시 빌려서 해석합니다. 오류 때 buffer/size는 무시하고, 조각을 모으거나 수신 원본을 파일에 쓰지 않습니다. 분석 출력은 byte count, printable/control/high-byte 개수와 고정된 후보·완전성 label뿐입니다. hex, ASCII 원문, 문자열, QMI client/transaction/message ID, TLV 값, IMEI·IMSI·전화번호·가입자/클라이언트 식별값·인증 payload는 출력하지 않습니다. printable 비율만으로 AT라고 판단하지 않습니다.

- `candidate=qmux-qmi`: [공식 libqmi 1.36.0 qmi-message.h](https://github.com/linux-mobile-broadband/libqmi/blob/1.36.0/src/libqmi-glib/qmi-message.h)의 marker `0x01`과 [qmi-message.c](https://github.com/linux-mobile-broadband/libqmi/blob/1.36.0/src/libqmi-glib/qmi-message.c)의 packed QMUX 5바이트·CTL 6바이트/서비스 7바이트 헤더, little-endian 길이, TLV 3바이트 헤더 및 경계 검사에 맞는 **형식 후보**입니다. 최소 전체 QMUX 헤더가 있어야 후보가 되며, QMUX 길이는 marker를 제외합니다. QMI TLV 길이와 QMUX 길이의 일치, 각 TLV 범위도 검사합니다. 서비스·메시지 의미나 flag 유효성은 확인하지 않으며 식별값을 해석하지 않습니다.
- `candidate=at-fixed-token`: 읽기 전체가 CRLF로 둘러싸인 정확한 `OK`, `ERROR`, 또는 `+CME ERROR:` 뒤 한 줄 printable 내용과 일치할 때만 붙입니다. 결과 내용은 출력하지 않습니다. 부분 문자열이나 이어 붙인 여러 줄은 판정하지 않습니다.
- `candidate=crlf-line`: 정확한 AT 토큰이 아닐 때만 봅니다. 읽기 하나 전체가 CR LF, printable ASCII(`0x20`~`0x7e`) 1자 이상, CR LF일 때만 붙입니다. 추가 출력은 `line_length=<숫자>`와 첫 글자 분류 `leading=plus|caret|upper|digit|other`뿐입니다. 내용, 문자열, hex, 나머지 글자 분류는 출력하지 않습니다. 빈 줄, CR/LF 단독, 내부 CR/LF·control·`0x7f`·high byte, 앞뒤 추가 바이트, 여러 줄, 잘린 조각은 unknown입니다. 한 줄 텍스트 구조 후보일 뿐 AT 포트나 프로토콜 증거가 아닙니다.
- `completeness=complete/incomplete`: **해당 읽기 하나**에서 형식 길이가 완전한지/더 필요한지입니다. 잘린 QMUX 후보는 검사 가능한 필드가 모순되지 않을 때만 incomplete로 표시합니다. 오류 길이·trailing bytes·맞지 않는 데이터는 `candidate=unknown`입니다. AT 토큰·CRLF 줄 조각은 unknown이고, 서로 다른 읽기를 결합하지 않습니다. 전부 무수신이면 `analysis status=no-data`로 표시합니다.

후보·완전성·통계는 실제 프로토콜, 특정 칩셋, 드라이버 대응, LTE 지원의 증거가 아닙니다. `evidence=format-only protocol=unconfirmed`를 항상 남깁니다. unknown도 지원하지 않는다는 결론이 아닙니다. 이전 16바이트는 보존하지 않아 사후 분석할 수 없고 재관측에서 같은 데이터가 나오지 않을 수 있습니다. 분석 모드 실장치 실행은 독립 검토 뒤 coordinator가 진행합니다.

## 재연결 대기

`--wait-reconnect --observe --analyze`만 허용합니다. 빠진 옵션·다른 순서·중복·추가 인자·fixture/help 혼용은 장치 접근 전에 종료 코드 2로 거부합니다. 시작 시 VID `2ca3`/PID `4009`의 단일 registry ID를 기준으로 잡습니다. 장치가 없으면 첫 새 연결을 기다리고, 이미 있으면 **그 ID는 읽지 않고 다른 ID**를 기다립니다. 대기 중 분리는 허용하지만 대상이 여러 개면 즉시 코드 4로 중단합니다.

대기는 registry 검색·숫자 VID/PID 속성·ID 조회만 사용합니다. plugin/accessor 생성, 장치 열기, USB 요청은 하지 않습니다. monotonic clock 기준 최대 90초, 100ms 간격이며 새 연결이 없으면 코드 13, SIGINT/SIGTERM flag 취소면 코드 11입니다. 동기 OS/kernel 메타데이터 호출에는 강제 timeout이 없어 프로세스 전체가 반드시 90초 안에 끝난다는 보장은 아닙니다. deadline 뒤 반환된 ID는 분석하지 않습니다.

감지한 ID를 기존 discovery에 넘겨 열기 전에 다시 대조합니다. 다른 세대·분리·접근 실패면 중단하고 다른 장치를 대신 읽지 않습니다. 이후에는 기존 2초/인터페이스·15초/전체·64KiB/전체·8회/인터페이스 상한과 메모리 분석·개인정보 경계를 그대로 적용합니다. OUT/control, seize, config/alt 변경, reset/clear-halt/detach, 소프트웨어 재연결·전원 변경, sudo는 추가하지 않습니다.

2026-10-14 오프라인 일반·sanitizer 테스트는 각각 27,508 checks, 실패 0이며 취소 신호 전환 회귀 검사와 독립 재검증을 통과했습니다. 대기와 세대 경합은 fake registry/clock으로 검사했습니다.

같은 날 실장치 재연결·초기 메시지 실험은 실행하지 않고 보류했습니다. 이번 단계에서는 실제 wait/cache/observe를 모두 실행하지 않았습니다. USB 관찰은 LTE 접속 검증과 별개이며, 유심 없는 장치의 동작은 확인하지 않았습니다. 첫 관찰의 16바이트를 보존하거나 재구성하지 않았으며 새 연결에서 같은 데이터가 나온다고 보장하지 않습니다.

### 수동 재연결 직후 관측 (2026-10-14)

사용자가 동글을 직접 다시 연결한 뒤 `--wait-reconnect` 없이 `--observe --analyze`를 한 번 실행했습니다. 새 registry ID 세대였고, 연결 뒤 이 도구가 처음 연 관찰입니다. 연결과 실행 사이 정확한 경과 시간은 측정하지 않았습니다. 유심 장착 여부는 기록하지 않았습니다.

| 인터페이스 | 수신 | 분석 |
|---|---|---|
| 0 | 16바이트 | printable 2, control 11, high 3, `candidate=unknown` |
| 1 | 없음 | timeout |
| 2 | 14바이트 | printable 10, control 4, high 0, `candidate=unknown` |
| 3 | 14바이트 | printable 10, control 4, high 0, `candidate=unknown` |
| 4 | 없음 | timeout |

전체는 44바이트·8회 읽기·5회 timeout·10,940ms, 종료 코드 0, stderr는 비어 있었습니다. raw 내용은 출력·저장하지 않았습니다.

같은 세대를 이미 읽은 2026-10-14 재관측은 0바이트였으므로, 연결 초기에만 데이터가 온다는 가설과 맞습니다. 확정은 아닙니다. 인터페이스 2·3은 같은 길이·구성이어서 CRLF로 감싼 한 줄 텍스트일 수 있지만, 분석기의 고정 AT 토큰 검사에는 해당하지 않았으므로 AT 포트라고 판정하지 않습니다. 인터페이스 0은 QMUX 형식 후보가 아니었습니다. 송신, LTE 접속, 네트워크 변경은 하지 않았습니다.

### crlf-line 검사 뒤 재연결 관측 (2026-10-14)

`crlf-line` 후보를 추가한 뒤 사용자가 동글을 한 번 더 뺐다 꽂았고, 새 registry ID 세대에서 `--observe --analyze`를 한 번 실행했습니다. 연결과 실행 사이 경과 시간은 측정하지 않았습니다.

| 인터페이스 | 수신 | 분석 |
|---|---|---|
| 0 | 16바이트 | printable 2, control 11, high 3, `candidate=unknown` |
| 1 | 없음 | timeout |
| 2 | 14바이트 | `candidate=crlf-line completeness=complete line_length=10 leading=plus` |
| 3 | 14바이트 | `candidate=crlf-line completeness=complete line_length=10 leading=plus` |
| 4 | 없음 | timeout |

전체는 44바이트·8회 읽기·5회 timeout·11,231ms, 종료 코드 0, stderr는 비어 있었습니다. 인터페이스별 바이트 수와 구성은 직전 수동 재연결 관측과 같았습니다. raw 내용은 출력·저장하지 않았습니다.

인터페이스 2·3은 읽기 하나 전체가 CRLF로 감싼 `+`로 시작하는 10자 한 줄이었습니다. 이 모양은 모뎀의 비요청 AT 결과 코드와 같은 형태이지만, 형식 일치만 확인했으므로 AT 명령 포트라고 확정하지 않습니다. 내용과 의미는 확인하지 않았습니다. 송신, LTE 접속, 네트워크 변경은 하지 않았습니다.

## AT 1회 송신

`--probe-at`은 사용자가 따로 승인한 단독 모드입니다. 근거는 두 번의 물리 재연결 직후 인터페이스 2·3에서 받은 읽기뿐입니다. 두 번 모두 14바이트·printable 10·control 4로 같았고, 그중 두 번째 관측에서 `crlf-line leading=plus` 형식 후보로 분류됐습니다. AT 포트로 확정된 상태에서 만든 기능이 아닙니다. 다른 옵션이나 인자와 함께 쓰면 장치에 접근하지 않고 종료 코드 2로 끝납니다.

`--observe`와 같은 단일 장치·설명자 검증을 통과해야 하고, 인터페이스 2만 seize 없이 엽니다. 열린 뒤 `GetPipeProperties`로 BULK OUT `0x03`과 BULK IN `0x84`(둘 다 maxPacket 512)가 정확히 하나씩인지 확인합니다. 다르거나 없으면 아무것도 보내지 않고 닫습니다(`write=skipped`). 송신 전에는 IN을 읽지 않습니다.

확인되면 소스에 고정된 3바이트 `AT\r`를 `WritePipeTO`로 한 번만 보냅니다(timeout 1000ms). 바이트·길이·인터페이스는 외부 입력으로 바꿀 수 없습니다. SDK의 `WritePipeTO`는 보낸 바이트 수를 알려 주지 않으므로 `write=ok`는 SDK가 성공을 반환했다는 뜻일 뿐이고, 함께 출력되는 `bytes=3`은 요청한 길이이지 장치가 받은 바이트 수가 아닙니다. 송신이 timeout이나 오류로 끝나면 재시도·`ClearPipeStall`·읽기 없이 닫고 중단합니다(`delivery=unknown`, timeout은 종료 코드 10). SDK 주석대로 이때 data toggle이 어긋날 수 있으니, 다시 통신하기 전에 동글을 물리적으로 재연결하세요.

송신이 성공한 뒤에만 같은 IN pipe를 송신 완료 시점부터 최대 2초, 최대 4회, 회당 512바이트 읽습니다. 각 읽기는 `probe_reply=ok`(정확히 `\r\nOK\r\n`), `echo-ok`(`AT\r` 에코 뒤 정확히 `\r\nOK\r\n`), `error`, `crlf-line`, `unknown` 중 하나로 분류하고, 읽기가 없으면 `none`입니다. 기존 수신 분석 줄도 함께 출력합니다. 응답 내용·hex·문자열은 출력하거나 저장하지 않으며 여러 읽기를 결합하지 않습니다.

수신 큐에 남아 있던 비요청 메시지가 응답으로 분류될 수 있고, 나뉘어 온 응답은 `unknown`이나 `crlf-line`으로 보일 수 있습니다. 결과는 형식 증거일 뿐 프로토콜 확정이 아닙니다(`protocol=unconfirmed`). 다른 AT 명령, 다른 인터페이스 송신, control/vendor 요청, 설정 변경, reset, detach, LTE·네트워크 변경은 하지 않습니다. `verify-offline`은 소스 전체에서 금지 USB API가 없는지와 송신 호출이 한 곳뿐인지 검사합니다. 실장치 실행은 독립 검토 뒤 coordinator가 진행합니다.

### 실장치 AT 응답 (2026-10-15)

독립 검토와 커밋 뒤 coordinator가 `--probe-at`을 한 번 실행했습니다. 직전 재연결 관측과 같은 registry ID 세대였습니다.

| 단계 | 결과 |
|---|---|
| 송신 | 인터페이스 2, OUT `0x03`, IN `0x84`, `write=ok bytes=3` |
| 읽기 1 | 20바이트, `probe_reply=unknown` (printable 14, control 6, high 0) |
| 읽기 2 | 9바이트, `probe_reply=echo-ok` |
| 읽기 3 | timeout |

최종 줄은 `probe status=0 sent=1 reads=2 reply=echo-ok`, 종료 코드 0, stderr는 비어 있었습니다. 응답 내용은 출력·저장하지 않았습니다.

읽기 2는 읽기 하나 전체가 `AT\r` 에코 뒤 정확히 `\r\nOK\r\n`인 형태였습니다. V.250 모뎀의 기본 에코·결과 코드와 같은 모양이므로 인터페이스 2가 AT 명령 포트일 가능성이 높습니다. 형식 일치만 확인했으므로 `protocol=unconfirmed`로 남깁니다. 읽기 1은 송신 전 수신 큐에 남아 있던 비요청 메시지일 수 있지만 확인하지 않았습니다. 인터페이스 3, 다른 AT 명령, LTE 접속, 네트워크 변경은 시험하지 않았습니다.

## ATI 1회 송신

`--probe-ati`는 위 `AT` 응답을 근거로 사용자가 따로 승인한 단독 모드입니다. `--probe-at`을 포함해 다른 옵션이나 인자와 함께 쓰면 장치에 접근하지 않고 종료 코드 2로 끝납니다. 장치·설명자 검증, 인터페이스 2의 seize 없는 열기, BULK OUT `0x03`/IN `0x84` 확인, 송신 전 IN 미읽기, 송신 실패 시 재시도·`ClearPipeStall`·읽기 없이 닫기, 송신 뒤 최대 2초·4회·회당 512바이트 읽기는 `--probe-at`과 같습니다. 송신 바이트는 소스에 고정된 4바이트 `ATI\r`이며 외부 입력으로 바꿀 수 없습니다. 소스 전체의 송신 호출은 여전히 한 곳입니다.

이 모드만 응답 내용을 출력합니다. 성공한 읽기를 메모리에서 최대 2048바이트까지 순서대로 이어 붙이고, 넘는 분량은 버린 뒤 `truncated=1`로 표시합니다. 이어 붙인 내용은 CR/LF로 나눠 빈 줄을 빼고 최대 16줄까지 출력합니다.

- 영문자, 숫자, 공백, `. , : ; - _ / + ( ) = #`로만 된 줄만 최대 80자까지 `text="..."`로 보입니다. 넘으면 `cut=1`입니다.
- 8자리 이상 이어지는 숫자는 `<digits:N>`로 가립니다.
- 모은 버퍼의 첫 바이트나 마지막 바이트에 닿은 숫자열은 원래 길이를 알 수 없으므로 길이와 상관없이 `<digits:N+>`로 가립니다. 그래서 경계에 걸린 짧은 숫자도 가려질 수 있습니다.
- CR/LF로 끝나지 않은 마지막 조각에는 `partial=1`을 붙입니다.
- 그 밖의 문자가 있는 줄은 내용 없이 `redacted=disallowed length=L`만 표시합니다.
- 송신 에코는 `echo=1`, `OK`/`ERROR`/`+CME ERROR`는 `result=ok|error`로만 표시하고 오류 내용은 출력하지 않습니다.
- 16줄을 넘으면 나머지 줄은 출력하지 않고 `ati lines_omitted=N`으로 개수만 표시합니다.
- 마지막 줄은 `probe status=<code> sent=<0|1> reads=N lines=M result=<ok|error|none> truncated=<0|1> evidence=format-only protocol=unconfirmed`입니다. `result`는 마지막 `OK`/`ERROR` 줄을 따르며, 출력하지 않은 줄도 셉니다.

현재 읽기 상한(4회×512바이트)이 버퍼 크기 2048바이트와 같아서 `truncated=1`은 지금 상한에서는 나오지 않는 방어 장치입니다.

raw 바이트, hex, 파일 저장은 없습니다. 7자리 이하 숫자나 영문자가 섞인 식별값은 가림 규칙에 해당하지 않으므로, 출력을 공유하기 전에 내용을 확인하세요. 수신 큐에 남은 비요청 메시지가 응답과 섞일 수 있어 결과는 `protocol=unconfirmed`로 남깁니다. 다른 명령, 반복 송신, 다른 인터페이스 송신, LTE·네트워크 변경은 하지 않습니다.

### 실장치 ATI 응답 (2026-10-15)

커밋 `6780663` 뒤 coordinator가 `--probe-ati`를 한 번 실행했습니다. 실행 파일은 커밋한 소스와 같은 소스에서 빌드한 것이었고(파일 시각과 소스 diff로 확인), 직전 `--probe-at`과 같은 registry ID 세대였습니다. 송신은 `write=ok bytes=4`, 읽기는 4바이트·150바이트 두 번 뒤 timeout이었습니다. 종료 코드 0, stderr는 비어 있었습니다.

| 줄 | 출력 |
|---|---|
| 1 | `echo=1` |
| 2 | `Manufacturer: Fibocom Wireless Inc.` |
| 3 | `Model: NL668T-GL` |
| 4 | `Revision: 19906.5090.00.02.00.23` |
| 5 | `ESN: +GSN: 0x0` |
| 6 | `+GCAP: +CGSM` |
| 7 | `IMEI: <digits:15>` (가림) |
| 8 | `result=ok` |

최종 줄은 `probe status=0 sent=1 reads=2 lines=8 result=ok truncated=0`입니다. IMEI 원문은 출력·저장하지 않았습니다.

이 응답은 표준 에코, V.250 `OK`, 3GPP TS 27.007 `+GCAP`·`+GSN` 형식과 맞으므로 인터페이스 2를 AT 명령 포트로 봅니다. 장치가 스스로 밝힌 모듈은 Fibocom NL668T-GL입니다. 다만 이것은 응답 문자열의 자기 보고이며, 하드웨어를 직접 확인한 것은 아닙니다. 인터페이스 0·1·3·4의 역할, 데이터 경로, 유심 상태, LTE 접속은 아직 확인하지 않았습니다. 다른 AT 명령과 USB 구성 변경도 보내지 않았습니다.

### 공개 소스와 대조한 인터페이스 역할 (2026-10-15)

Fibocom의 공식 NL668 AT 매뉴얼과 USB 문서는 찾지 못했습니다. 대신 Fibocom 기본 ID `1508:1001`(NL668)을 다루는 공개 소스를 coordinator가 원문으로 내려받아 확인했습니다.

- [Linux `qmi_wwan.c`](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/net/usb/qmi_wwan.c): `QMI_QUIRK_SET_DTR(0x1508, 0x1001, 4)` `/* Fibocom NL668 series */` — 이 Linux 장치 항목은 interface 4에 DTR 동작을 적용합니다. 해당 quirk는 CDC `SET_CONTROL_LINE_STATE` 요청으로 DTR 상태를 바꿉니다. 이는 `1508:1001` Linux 경로의 동작이며 DJI 장치에서 같은 동작이 필요한지는 확인하지 않았습니다.
- [Linux `option.c`](https://github.com/torvalds/linux/blob/0fcb05b72344f40806cbe810de8872f1698bf053/drivers/usb/serial/option.c): `USB_DEVICE(0x1508, 0x1001)` `/* Fibocom NL668 (IOT version) */`, `RSVD(4) | RSVD(5) | RSVD(6)` — 예약 비트는 해당 드라이버의 포트 처리 정책이며, 실제 장치에 해당 인터페이스가 있다는 증거는 아닙니다.
- [ModemManager `77-mm-fibocom-port-types.rules`](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/blob/main/src/plugins/fibocom/77-mm-fibocom-port-types.rules) `# Fibocom NL668-EAU`: 해당 규칙은 인터페이스 0을 QCDM/DIAG, 1을 AT primary, 2·3을 AT secondary로 분류합니다.
- 공개 Linux 소스는 `1508:1001` 장치에 관한 구현 근거이며, 이를 DJI `2ca3:4009` 장치의 동작으로 일반화하지 않습니다.

| 인터페이스 | 이 동글 | 공개 소스의 NL668 | 판단 |
|---|---|---|---|
| 0 | ff/ff/ff, BULK만 | DIAG/QCDM | 배치 일치. 재연결 직후 16바이트가 진단 프레임인지는 미확인 |
| 1~3 | ff/00/00, INT+BULK | AT 포트 | 인터페이스 2는 실장치로 확인, 3은 비슷한 형식의 시작줄을 관측했으나 원문을 보존하지 않아 같은 내용인지는 확인할 수 없음 |
| 4 | ff/ff/ff, INT(8)+BULK | QMI, Linux 항목은 DTR 동작 적용 | 배치 일치. 실제 QMI 동작과 DTR 필요 여부는 미확인 |

이 동글의 VID/PID는 DJI의 `2ca3:4009`이므로 공개 소스의 `1508:1001` 항목과 직접 일치하지 않습니다. 커널 항목은 인터페이스 5·6을 예약하지만, 그 비트만으로 DJI 장치에 실제 인터페이스가 있음을 뜻하지 않습니다. 따라서 표의 대응은 근거 있는 추정이며 확정이 아닙니다.

### 데이터 경로 후보와 확인 범위

다음 오프라인 구현 단계는 [PPP 연결 설계](docs/ppp-design.md)를 따르는 production과 분리된 전송 core 및 fake 테스트입니다. 장치 통신, `pppd` 실행, 네트워크 변경은 이번 구현 단계에 포함하지 않습니다.

Fibocom의 [일반 NL668 제품 페이지](https://www.fibocom.com/en/SeriesProduct/info_itemid_121.html)는 USB 2.0과 PPP/ECM/RNDIS/GobiNet/QMI_WWAN 프로토콜 목록을 제시합니다. 이 일반 제품 정보만으로 NL668T-GL 또는 DJI VID/PID 장치에서 특정 모드가 지원되거나 활성화된다고 단정할 수 없습니다. 공식 NL668T-GL AT 명령어·USB 모드 전환 문법과 저장·복원 절차는 확인하지 못했습니다. 공개 원문을 찾지 못했다는 사실은 기능 미지원을 뜻하지 않습니다.

| 경로 | 확인된 근거 | 아직 확인하지 않은 점 |
|---|---|---|
| QMI | 이 동글의 인터페이스 4 배치와 Linux `1508:1001` 항목이 맞아 가장 강한 후보입니다. 확인한 Linux 구현은 CDC-WDM의 control/status 경로와 bulk packet data 경로를 분리합니다. 드라이버의 `raw_ip`는 host-side 설정이며 DJI 장치의 framing을 확인해 주지 않습니다. | 이 DJI 장치에서의 QMI 동작·제어 및 데이터 형식 협상·활성 상태, 필요한 초기화/DTR, macOS 드라이버·접속 경로 |
| PPP | Fibocom의 일반 제품 페이지에 PPP가 기재돼 있고, 이번 `AT+CGDATA=?` 지원 조회에서 이 모듈이 `PPP`를 capability로 보고했습니다. PPP는 직렬 링크를 쓰는 경로입니다. | 이 동글에서 실제 PPP 접속이 되는지, AT 포트·다이얼 명령과 실제 USB 전송 연결 |
| ECM/RNDIS | 일반 NL668 제품 페이지의 프로토콜 목록에 있습니다. | 현재 USB 구성에서 활성인지, 모드 변경·복원 문법 및 절차 |

이번 재연결 뒤 기본 캐시 조회에서 동일 VID/PID의 registry ID `0x10059bfdc`, 209바이트 구성, 인터페이스 5개를 확인했습니다. 기본 조회는 캐시 설명자만 읽었으며 USB 장치를 열거나 전송하지 않았습니다. 이 결과는 네트워크 인터페이스나 데이터 경로의 존재를 확인하지 않습니다. 앞선 관측에서는 이 동글용 macOS 네트워크·직렬 포트 노드를 확인하지 못했으며, 이번 캐시 조회에서는 노드 생성 여부를 다시 검사하지 않았습니다. `pppd`는 직렬 링크를 전제로 합니다. 이를 USB bulk와 연결하는 stdio/PTY 전송 다리는 구현하지 않았고, `pppd`도 실행하지 않았습니다. QMI 제어·상태 경로를 확인하려면 control endpoint 요청이 필요합니다. BULK IN 관측은 control 응답을 읽은 것이 아니며, control 요청은 현재 승인된 접근 범위 밖입니다. IOKit control API가 존재한다는 사실도 이 장치가 해당 제어를 지원한다는 증거가 아닙니다.

업스트림 Linux 소스에서 DJI `2ca3:4009`의 일치 항목은 확인하지 못했지만, 이는 해당 지원이 없다는 뜻은 아닙니다. 따라서 QMI는 우선 조사할 후보이고 PPP·ECM/RNDIS도 일반 제품 정보상 가능한 후보지만, 실제 데이터 경로는 아직 정하지 않았습니다. 공식 제어 문서와 데이터 경로 증거를 확인하기 전에는 USB 모드 변경이나 데이터 접속을 시도하지 않습니다.

확인 후보였던 것은 [ETSI TS 127 007 V19.6.0 (3GPP TS 27.007) §10.1.12](https://www.etsi.org/deliver/etsi_ts/127000_127099/127007/19.06.00_60/ts_127007v190600p.pdf)의 `AT+CGDATA=?` 지원 조회입니다. 표준은 지원하는 `<L2P>` 목록을 반환하도록 정의하며, PPP가 포함될 수 있습니다. `D` 다이얼 명령으로 Packet Domain 동작을 지정할 수 있는 경우에는 이 명령의 구현을 선택 사항으로 둡니다. 아래 보존 관측에서 고정 지원 조회를 한 번 실행했지만, 목록 보고는 실제 PPP 접속을 증명하지 않습니다. 실제 `AT+CGDATA=<L2P>` 실행은 PDP 활성화와 온라인 상태 진입을 일으킬 수 있으므로 이 조사 범위에 포함하지 않습니다. `ATD` 다이얼과 USB 모드 변경도 별도 승인과 근거 확인 전에는 실행하지 않습니다.

## 상태 조회

`--probe-status`는 사용자가 따로 승인한 단독 모드입니다. 다른 probe 모드를 포함해 다른 옵션이나 인자와 함께 쓰면 장치에 접근하지 않고 종료 코드 2로 끝납니다. 장치·설명자 검증, 인터페이스 2의 seize 없는 열기, BULK OUT `0x03`/IN `0x84` 확인은 다른 probe 모드와 같습니다.

상태를 바꾸지 않는 고정 조회 명령 `AT+CPIN?`, `AT+CSQ`, `AT+COPS?`, `AT+CEREG?`를 이 순서로 한 번씩 보냅니다. 명령 바이트·순서·횟수는 소스 상수로 고정돼 있고, 송신 함수는 이 상수 표에 있는 버퍼만 받습니다. 두 번째 명령부터는 송신 전에 장치와 pipe를 다시 확인합니다. 송신이 실패하거나 timeout이 나면 그 자리에서 멈추고 남은 명령은 보내지 않습니다. 재시도·`ClearPipeStall`·읽기 없이 닫습니다.

송신 전에는 IN pipe를 읽지 않습니다. 그래서 큐에 남아 있던 이전 응답은 비우지 않고, 자기 에코보다 앞선 완결된 줄로 들어올 수 있습니다. 실행 경로에서는 그 줄을 내용을 해석하지 않고 길이만 세어 `pre_echo_skipped=N`으로 건너뜁니다. 그 줄이 자기 prefix여도 내용은 출력하지 않고 건너뛰되, 뒤에 자기 에코가 오면 `mismatch`(stale-before-echo)로 표시해 값을 결과로 쓰지 않습니다. 자기 에코가 끝내 오지 않으면 `no-echo` 미확인입니다. 건너뛰는 것은 장치 큐를 비우는 동작이 아닙니다.

송신이 성공하면 명령마다 최대 2초·4회·회당 512바이트를 읽습니다. 2초와 4회는 명목상 상한이며, 동기 SDK 호출의 wall-clock 시간을 보장하지 않습니다. 읽기를 끝내는 조건은 자기 에코 뒤에 오는 CR/LF로 끝난 최종 결과 줄입니다. 자기 에코보다 앞선 최종 결과 줄로는 끝내지 않습니다. 출력은 명령별로 정한 필드뿐입니다.

어느 명령이든 다음이면 그 실행의 나머지 명령은 보내지 않습니다. 읽기 timeout, 읽기·검증 오류, 취소, 분리, clock 오류, 2초 예산 소진, 4회 상한, 0바이트 성공 읽기, 버퍼보다 큰 읽기 보고, 귀속 미확인, 그리고 `ERROR`·`+CME ERROR`로 끝난 응답입니다. timeout은 조용한 결과로 넘기지 않습니다. SDK 주석대로 timeout 뒤 data toggle 재동기화가 필요할 수 있어, 같은 실행에서는 다음 읽기·쓰기·재검증 없이 close·release만 합니다. 다음 실행이 안전하다는 보장은 없으므로 다시 통신하기 전에 동글을 물리적으로 재연결하세요. 이 중단은 종료 코드가 아니라 고정 `stop=<사유> stop_index=<슬롯>`으로 구분합니다. 논리적인 미확인(timeout, 예산, 상한, 0바이트, 귀속 미확인, 최종 오류)은 기존처럼 `DJI_OK`를 유지하고, I/O 오류만 기존 오류 코드를 반환합니다. 보내지 않은 슬롯은 `sent=0`이며 성공으로 표시하지 않습니다.

| 명령 | 출력 |
|---|---|
| `AT+CPIN?` | 유심 상태 코드(대문자·숫자·공백, 대문자 1개 이상, 20자 이하). `READY`, `SIM PIN` 같은 상태 단어 |
| `AT+CSQ` | `rssi`, `ber` 숫자 |
| `AT+COPS?` | `mode`, `format`, 통신사 이름 또는 MCC+MNC, `act`. 이름은 영문자·숫자·공백과 `- . & + / ' _ ( )`, 32자 이하 |
| `AT+CEREG?` | `n`, `stat`만. 그 뒤 필드(TAC·CI 등)는 개수만 `extra_fields=K` |
| `+CME ERROR` | 1~3자리 숫자 코드, 또는 숫자 없는 영문 단어로만 된 40자 이하 문구 |

형식이 다르거나 식별값처럼 보이는 값(필드 안 숫자가 합쳐서 7개 이상인 경우 등)은 내용 없이 `redacted=1 length=L`로 표시합니다. 해당 명령과 다른 비요청 줄은 `unsolicited=1 length=L`, 그 밖의 줄은 `other=1 length=L`, CR/LF 없이 끝난 조각은 `partial=1 length=L`로 길이만 표시합니다. raw 바이트·hex·파일 저장은 없습니다. PIN 입력, 설정 변경, LTE·네트워크 변경은 하지 않습니다.

자기 에코 뒤에 최종 결과 줄이 오기 전에 다른 줄이 있으면 그 명령은 미확인으로 끝나고, 다음 명령은 보내지 않습니다. 그래서 늦은 응답이 다음 명령의 결과로 기록되지는 않습니다. 반대로 자기 에코보다 앞선 줄은 실행 경로에서 건너뛰므로, 그 줄이 자기 prefix가 아닌 한 판정을 막지 않습니다. 같은 명령의 잔여 에코와 잔여 응답은 방금 보낸 요청과 모양이 같아 완전히 가려 내지 못합니다. `matched`여도 요청 귀속의 완전한 증명은 아닙니다. 에코를 끈 모뎀은 정상 응답도 `no-echo`로 미확인입니다. 실행 경로에서는 에코가 오지 않으면 응답 줄을 모두 건너뛴 채 기다리다 첫 명령에서 실행을 멈춥니다. 멈춘 사유는 timeout, 4회 상한, 2초 예산 중 먼저 걸린 것(`stop=timeout`·`read-cap`·`time-budget`)이며, 건너뛴 내용은 출력하지 않습니다. 첫 슬롯 전에 장치 준비·열기·pipe 확인이 실패하면 `stop=io-error`, 준비와 열기 사이에 취소되면 `stop=cancelled`이며 `stop_index`는 첫 슬롯입니다. 장치 준비를 시작하기 전에 이미 취소된 경우는 `stop=cancelled stop_index=6`입니다. 결과는 형식 증거일 뿐 프로토콜 확정이 아닙니다(`protocol=unconfirmed`). SIM·망·APN·`NO CARRIER`의 원인은 이 조회로 확정하지 않습니다. 이 변경의 실기 검증은 아직 없습니다.

### 응답 귀속 판정

명령마다 받은 줄을 출력하기 전에 한 번 판정하고, 줄 출력 뒤에 고정 한 줄을 붙입니다. `--probe-pdp`와 `--probe-data-support`도 같은 판정을 씁니다. 읽기 오류·취소·연결 끊김과 버퍼 잘림은 줄 출력 전에 반영합니다. 그래서 `matched`를 출력한 뒤 판정이 바뀌는 일은 없습니다.

```
status cmd=<이름> attribution=<matched|mismatch|unproven> reason=<사유>
pdp attribution=... reason=...
data-support attribution=... reason=...
```

- `matched`(`reason=format-only`): CR/LF로 끝난 줄 기준으로 자기 에코가 정확히 한 번 첫 줄이고, 최종 결과 줄이 정확히 한 번 마지막 줄이며, 그 사이에 자기 prefix·`+CME ERROR` 말고 다른 줄이 없고, 조각·잘림·줄 생략·I/O 오류가 없을 때만입니다. **형식이 맞는다는 뜻일 뿐 지금 보낸 요청의 응답이라는 증명이 아닙니다.** 같은 모양의 잔여 응답이 큐에 남아 있었을 가능성은 배제하지 못합니다.
- `mismatch`: 다른 명령의 에코나 `AT`로 시작하는 다른 줄(`foreign-echo`), 자기 에코 중복(`dup-echo`), 자기 에코 앞의 최종 결과 줄(`final-before-echo`)이나 알려진 prefix·`+CMS ERROR:`·`+`로 시작하지 않는 일반 줄(`stale-before-echo`), 다른 조회 명령의 알려진 prefix(`foreign-prefix`), 최종 결과 줄 여러 개(`multi-final`)입니다. 사유는 이 순서로 하나만 고릅니다. 응답 모양이 요청과 어긋난다는 뜻이며 SIM·망·APN 원인을 가리키지 않습니다. 이때 자기 prefix 줄은 값을 해석하지 않고 `withheld=1 length=L`만 출력하며 `contexts`·`support_lists`에 세지 않습니다.
- `unproven`: 귀속을 판단할 근거가 부족합니다. 사유는 `io-error`, `truncated`, `omitted`, `no-echo`, `partial`, `no-final`, `trailing`, `cms-error`, `unlisted-prefix`, `other-line` 순으로 하나만 고릅니다. 어떤 mismatch 사유도 unproven 사유보다 앞섭니다. 기존 필터를 통과한 값은 그대로 출력할 수 있지만 귀속은 확인되지 않은 것입니다.
- 에코를 끈 모뎀(`ATE0`)이면 응답이 정상이어도 늘 `no-echo`로 `unproven`입니다. 알려진 prefix에 없는 `+` 줄은 자기 에코 뒤에서는 다른 명령의 응답으로 단정하지 않고 `unlisted-prefix`로 둡니다. 이 줄만으로는 `mismatch`가 되지 않고, 다른 mismatch 근거가 함께 있으면 그 사유가 앞섭니다. `+CMS ERROR:`는 읽기를 끝내는 최종 결과 줄로 보지 않습니다.
- 직접 파서(`status_print_lines`)와 실행 경로의 판정은 다릅니다. 직접 파서는 자기 에코 앞의 완결된 줄을 모두 분석해 엄격한 의미를 유지합니다(최종 결과 줄이면 `final-before-echo`, 그 밖은 `stale-before-echo`). 실행 경로만 그 줄을 내용 분석 없이 건너뛰고, 그중 자기 prefix만 `mismatch`로 남깁니다. 자기 에코 중복과 자기 에코 뒤의 다른 줄 판정은 양쪽이 같습니다.
- 현재 수집 버퍼(2048바이트)는 읽기 상한(4회 × 512바이트)과 같아 실제 실행에서 `truncated`에 도달하지 않습니다. 판정 경로만 남겨 두었고 실행으로 검증하지 않았습니다.

요약 줄 `status cmd=... sent=... reads=... result=...`, `contexts=M`, `support_lists=K`의 형식은 바뀌지 않았습니다. `result=ok`나 `contexts=0`은 그 창에서 관측한 파싱 결과일 뿐 귀속을 뜻하지 않으므로 판정 줄과 함께 읽으세요. 개인정보 필터와 길이만 표시하는 규칙은 그대로입니다. 고정 명령 표, 송신 순서·횟수, 명령당 읽기 4회·2초, 송신 전 IN 읽기 없음은 바꾸지 않았습니다. 실행 경로의 읽기 종료는 첫 최종 결과 줄이 아니라 자기 에코 뒤의 최종 결과 줄이며, 미확인이면 다음 명령을 보내지 않습니다. 최종 요약에 `stop=<사유> stop_index=<슬롯>`이 붙습니다.

### 실장치 상태 조회 (2026-10-15)

사용자가 유심을 다시 끼운 뒤, 커밋 `3f8b248`과 같은 소스로 빌드한 실행 파일로 `--probe-status`를 두 번 실행했습니다. 두 번 모두 같은 registry ID 세대였으므로 사이에 재연결은 없었던 것으로 봅니다(유심 재장착과 재연결 여부는 coordinator 관찰 기준). 두 실행 모두 4개 명령을 보냈고(`commands_sent=4`), 명령마다 `result=ok`였습니다. 종료 코드 0, stderr는 비어 있었습니다.

| 명령 | 첫 실행 | 두 번째 실행 |
|---|---|---|
| `AT+CPIN?` | `cpin="READY"` | `cpin="READY"` |
| `AT+CSQ` | `rssi=99 ber=99` | `rssi=20 ber=99` |
| `AT+COPS?` | `mode=0` (통신사 필드 없음) | `redacted=1 length=20` |
| `AT+CEREG?` | `n=0 stat=0 extra_fields=0` | `n=0 stat=1 extra_fields=0` |

3GPP TS 27.007 기준으로 첫 실행은 유심은 준비됐지만 망 미등록이고 망 검색도 하지 않는 상태(`stat=0`)이며, 신호는 알 수 없음(`99`)입니다. 두 번째 실행은 홈 네트워크 등록(`stat=1`)이고, `rssi=20`은 약 -73 dBm입니다. 같은 세대 안에서 바뀌었으므로 모뎀이 기동 뒤 스스로 망에 등록한 것으로 보입니다. 시간 간격과 원인은 측정하지 않았습니다.

첫 실행의 CPIN 응답 앞에는 10자 비요청 한 줄(`unsolicited=1 length=10`)이 있었습니다. 재연결 직후 보던 줄과 길이가 같습니다. 두 번째 실행의 통신사 줄은 당시 필터 형식에 맞지 않아 내용 없이 길이만 나왔습니다. 기지국 위치 필드는 두 실행 모두 응답에 없었습니다(`n=0`, `extra_fields=0`). PIN 입력, 설정 변경, 데이터 접속은 하지 않았습니다.

이 기록 뒤 통신사 이름 필터를 넓혀 `+ / ' _ ( )`를 허용했습니다(예: `LG U+`, `Movistar (ES)`). 따옴표·쉼표·제어문자가 있는 이름은 필드 구조를 위장할 수 있어 계속 길이만 표시합니다. 두 번째 실행의 `redacted=1 length=20` 줄이 새 필터로 출력될지는 다시 실행해야 알 수 있습니다.

## 데이터 연결 설정 조회

`--probe-pdp`는 사용자가 따로 승인한 단독 모드입니다. 다른 probe 모드를 포함해 다른 옵션이나 인자와 함께 쓰면 장치에 접근하지 않고 종료 코드 2로 끝납니다. 장치·설명자 검증, 인터페이스 2의 seize 없는 열기, BULK OUT `0x03`/IN `0x84` 확인, 송신 실패 시 처리, 읽기 상한, 자기 에코 뒤 최종 결과 줄에서 읽기 종료, 미확인 시 실행 중단은 「상태 조회」와 같습니다.

상태를 바꾸지 않는 고정 조회 명령 `AT+CGDCONT?`를 한 번만 보냅니다. 모뎀에 저장된 데이터 연결 설정(PDP context)을 읽는 명령이며, 설정 변경이나 데이터 접속은 하지 않습니다. context마다 다음 한 줄을 출력합니다.

```
pdp line=N cid=<번호> type="<형식>" apn="<APN>" addr_present=<0|1> addr_length=<L> extra_fields=<K>
```

- `cid`는 1~2자리 숫자입니다. `type`은 3GPP 값 목록(`IP`, `IPV6`, `IPV4V6`, `PPP`, `X.25`, `OSPIH`, `NON-IP`, `ETHERNET`, `UNSTRUCTURED`)에 있을 때만 보이고, 그 밖은 `type=other type_length=L`입니다.
- APN은 영문자·숫자·`.`·`-`·`_`로 된 63자 이하이고 숫자가 합쳐서 7개 미만일 때만 보입니다. 숫자와 `.`만으로 된 값이나 IPv6 주소 모양인 값은 주소일 수 있어 가립니다. 빈 APN은 `apn=""`이고, 그 밖은 `apn=redacted apn_length=L`입니다.
- PDP 주소는 내용을 출력하지 않고 있는지와 길이만 표시합니다.
- 그 뒤 필드(d_comp, h_comp 등)는 따옴표 밖 쉼표 기준으로 개수만 셉니다.
- 형식이 깨진 줄은 `pdp line=N redacted=1 length=L`로 길이만 표시합니다.
- 에코·결과·CME·비요청·기타·조각 줄 처리와 16줄·2048바이트 상한은 「상태 조회」와 같습니다.

요약 줄에 `contexts=M`이 붙습니다. raw·hex·파일 저장은 없습니다. `+CGDCONT:` 형식의 비요청 메시지가 오면 응답으로 파싱될 수 있고, 송신 전 큐에 남은 응답이 섞일 수 있습니다. 이때도 같은 필터를 거칩니다. 표준 필드 순서(cid·형식·APN·주소)에서는 주소 내용이 출력되지 않습니다. 순서가 비표준인 응답에 대비해 형식은 값 목록으로 제한하고, 주소 모양 APN은 가립니다. 그래도 모든 비표준 형식을 막는다고 보장하지는 않습니다. 결과는 형식 증거일 뿐 프로토콜 확정이 아닙니다(`protocol=unconfirmed`).

### 실장치 데이터 연결 설정 조회 (2026-10-15)

커밋 `e94c55b`의 소스로 빌드한 실행 파일로 `--probe-pdp`를 한 번 실행했습니다. registry ID는 `0x100598f05`였고, 인터페이스 2의 OUT `0x03`/IN `0x84`를 사용했습니다. 고정 `AT+CGDCONT?` 명령 12바이트를 한 번 보냈습니다.

| 항목 | 결과 |
|---|---|
| 송신·읽기 | `write=ok bytes=12`, 읽기 2회: 12바이트·110바이트 |
| PDP context | `cid=1 type="IPV4V6" apn="<carrier APN>" addr_present=1 addr_length=44 extra_fields=4` |
| 요약 | `contexts=1 result=ok commands_sent=1`, 종료 코드 0, stderr 비어 있음 |

`APN`은 실장치에 설정된 사용자 승인 정보입니다. PDP 주소의 원문은 기록하지 않았습니다. `IPV4V6`는 설정값이며, 주소 할당이나 실제 인터넷·데이터 접속 성공을 뜻하지 않습니다. 앞서 망 등록 상태를 관측했더라도 데이터 접속 여부는 확인하지 않았습니다. 통신사 필터 확장은 구현됐지만 이번 실장치 실행에서 `AT+COPS?`를 다시 조회하지 않았습니다.

## 데이터 방식 지원 조회

`--probe-data-support`는 사용자가 따로 승인한 단독 모드입니다. `--help`에도 고정 12바이트·1회 송신과 실제 PPP 접속 미증명 경계를 표시합니다. 다른 probe 모드를 포함해 다른 옵션이나 인자와 함께 쓰면 장치에 접근하지 않고 종료 코드 2로 끝납니다. 장치·설명자 검증, 인터페이스 2의 seize 없는 열기, BULK OUT `0x03`/IN `0x84` 확인, 송신 실패 시 처리, 읽기 상한, 자기 에코 뒤 최종 결과 줄에서 읽기 종료, 미확인 시 실행 중단은 「상태 조회」와 같습니다.

고정 지원 조회 명령 `AT+CGDATA=?\r`(끝의 CR 포함 12바이트)를 한 번만 보냅니다. 송신 timeout은 1초이며 성공 뒤 읽기는 최대 4회·회당 512바이트·전체 2초입니다. timeout·오류 때 받은 버퍼와 크기는 무시합니다. 데이터 접속을 시작하는 `AT+CGDATA=...`, `ATD`, PDP 활성화, APN·USB 모드 변경, QMI·DTR 제어와 시스템 네트워크 변경은 하지 않습니다. 오류·timeout·미지원이면 재시도하지 않습니다. 표준은 `D` 다이얼 명령으로 Packet Domain 동작을 지정할 수 있는 경우 이 조회 구현을 선택 사항으로 둡니다. 따라서 조회 결과만으로 다른 데이터 방식이나 PPP 미지원을 판단하지 않습니다.

완전한 `+CGDATA: (<따옴표로 감싼 L2P 목록>)` 구조에서 알려진 값만 출력합니다(예: `+CGDATA: ("PPP","M-HEX","M-RAW_IP")`). L2P는 데이터 연결에 쓸 계층 2 프로토콜입니다. 이 세 값은 [ETSI TS 127 007 V19.6.0 §10.1.12](https://www.etsi.org/deliver/etsi_ts/127000_127099/127007/19.06.00_60/ts_127007v190600p.pdf)에 정의돼 있고, 필터는 이 목록만 허용합니다. 다른 표준 값이 있어도 확인 전에는 넣지 않습니다. 출력은 다음 한 줄입니다.

```
data-support line=N capability=reported entries=<개수> l2p="PPP,M-HEX,M-RAW_IP" ppp_connection=unproven
```

- 값은 소스 상수의 고정 순서로만 나옵니다. 응답 원문이나 중복은 출력하지 않습니다.
- 빈 목록, 따옴표·쉼표·괄호가 깨진 목록, 제어문자, 목록에 없는 이름은 목록 전체를 `redacted=1 length=L`로 가립니다. 그중 알려진 값의 일부도 출력하지 않습니다. 이런 지원 응답 뒤에는 해당 응답 묶음의 지원 목록 처리를 멈춥니다. 깨진 따옴표 안의 CR/LF 뒤 조각도 새 지원 증거로 보지 않습니다.
- 에코와 `OK`/`ERROR`는 label만 출력합니다. `+CME ERROR`도 코드·문구 없이 `result=cme-error`만 출력합니다. 최초 결과를 고정하고 같은 읽기에 붙은 후속 줄은 길이만 표시합니다. 후속 목록은 지원 증거로 세지 않습니다.
- CR/LF 없이 끝난 조각, 비요청 줄, 다른 줄은 길이만 출력합니다. 송신 전 큐에 남은 응답이나 비요청 메시지가 섞일 수 있으며, 이 한계는 「상태 조회」와 같습니다.
- 16줄·2048바이트 상한은 기존과 같습니다. 상한 밖 원문은 출력하지 않습니다.

요약의 `support_lists`는 필터를 통과해 출력한 목록 줄 수입니다. `entries`는 해당 목록에 나온 항목 수이며 중복도 셉니다. `OK`만 있고 지원 목록이 없으면 `support_lists=0`이며 PPP 지원으로 보지 않습니다. `ERROR`, CME, timeout, unknown은 미지원을 확정하지 않습니다. 이때 다른 명령이나 실행형 명령으로 바꾸지 않습니다. 목록에 `PPP`가 있어도 실제 PPP 접속이 된다는 증거가 아닙니다. 결과는 형식 증거일 뿐 프로토콜 확정이 아닙니다(`protocol=unconfirmed`).

### 보존 관측 (2026-10-15)

소스 커밋 `33e8d92` 이후 사용자의 선택에 따라 coordinator가 production `--probe-data-support`를 실제 장치에서 한 번 실행했습니다. 관측 파일은 `build/probe-data-support-device-20261015-zHXRue/{stdout,stderr,status}`이며, 실행 파일 binary mtime은 `04:49:57+0900`입니다. 파일 내용을 확인했고, 작성자 검사 뒤 source 변경이 없었습니다. 이 기록은 암호학적 provenance나 hash 검증을 뜻하지 않습니다.

| 항목 | 관측값 |
|---|---|
| 장치 | registry ID `0x10059bfdc`, VID `2ca3`, PID `4009`, 구성 `1`, 설명자 `209`바이트, 인터페이스 `5`개 |
| 송신 | `cmd=cgdata-test`, 인터페이스 2의 OUT `0x03`/IN `0x84`, `write=ok bytes=12` |
| 읽기 | `14`·`12`·`26`바이트, 총 3회 |
| 비요청·응답 | 비요청 줄 `length=10`(이 앱의 필터 로그에는 원문 없이 길이만 기록), 에코, `capability=reported entries=1 l2p="PPP" ppp_connection=unproven`, `result=ok` |
| 요약 | `sent=1 reads=3 support_lists=1`, `evidence=format-only protocol=unconfirmed`, 종료 코드 `0`, stderr 비어 있음 |

`bytes=12`는 요청한 송신 길이이며 장치가 실제로 받은 바이트 수를 증명하지 않습니다. `PPP`는 모듈이 보고한 capability 목록일 뿐 실제 PPP 접속·PPP bridge·`pppd` 실행·LTE/인터넷 접속을 증명하지 않습니다. 실행형 `AT+CGDATA=...`, `ATD`, PDP 활성화, USB 모드 변경, QMI·DTR 제어와 시스템 네트워크 변경은 하지 않았습니다. 따라서 PPP는 실제 모듈 capability 신호를 얻은 후보로 범위를 좁혔지만, QMI는 여전히 후보이며 연결 방식은 확정하지 않았습니다. 이번에 보존한 앱 stdout/stderr에는 비요청·알 수 없는 응답 원문이나 raw 바이트 덤프가 없고, 에코·결과는 label로 기록됐습니다.

## 파서와 신뢰 경계

`descriptor_parse()`는 실제 전달받은 버퍼 길이를 검사합니다. 첫 헤더 최소 9바이트, 전체 최대 4096바이트, `wTotalLength`와 실제 길이 일치, 모든 하위 설명자 경계와 개수를 검증합니다. 고유 인터페이스 번호와 alternate setting을 따로 셉니다. 같은 인터페이스/alternate 쌍과 같은 alternate 내부 endpoint 주소 중복, 인터페이스 앞 endpoint, 잘린 설명자를 거부합니다. 최대 packet 크기는 `wMaxPacketSize`의 하위 11비트이며 원래 필드도 보존합니다.

**IOKit 캐시는 별도 신뢰 경계입니다.** SDK의 `GetConfigurationDescriptorPtr`는 allocation 길이를 반환하지 않습니다. 이 경로는 OS/SDK가 소유한 kernel-trusted blob의 고정 헤더와 `wTotalLength`가 실제 allocation을 나타낸다고 신뢰합니다. 헤더와 최대 4096바이트 상한을 검사한 뒤 그 파생 길이를 파서에 넘깁니다. 이 상한이 손상된 SDK 포인터의 allocation 길이를 증명하는 것은 아닙니다. 파일이나 임의 사용자 포인터에 이 파생 길이 방식을 적용하지 않습니다.

fixture는 파일 자체를 최대 4096바이트로 제한하고 ASCII hex 및 공백(`space`, tab, CR, LF)만 허용합니다. 홀수 자릿수, NUL, 비ASCII, `0x` 접두사와 주석은 거부합니다. 파서는 구성 뒤 추가 바이트도 거부합니다.

## 빌드와 오프라인 테스트

새 패키지 설치는 필요하지 않습니다. 현재 사용한 도구는 설치된 `clang`, `make`, IOKit/CoreFoundation SDK입니다. `libusb 1.0.30`은 API 안전성 검토에만 사용했고 링크하지 않습니다.

빌드 전 `memory_pressure -Q`에서 free가 30% 이상인지 확인하세요. 공용 잠금 안에서 빌드와 테스트를 순서대로 실행합니다. 여러 빌드를 동시에 시작하지 마세요.

```sh
memory_pressure -Q
lockf -k -t 900 /tmp/claude-heavy.lock nice -n 10 make all
lockf -k -t 900 /tmp/claude-heavy.lock nice -n 10 make ppp-usb-sdk
lockf -k -t 900 /tmp/claude-heavy.lock nice -n 10 make test
lockf -k -t 900 /tmp/claude-heavy.lock nice -n 10 make test-sanitize
lockf -k -t 900 /tmp/claude-heavy.lock nice -n 10 make verify-offline
```

`src/offline_stream.h/c`는 생산 앱에 연결하지 않은 순수 C 오프라인 전송 모듈입니다. `src/ppp_child.h/c`는 실제 POSIX child/pipe transport, `src/ppp_supervisor.h/c`는 이 transport와 core 및 주입한 USB peer를 조율합니다. `make test`, `make test-sanitize`는 고정 로컬 `ppp_child_fixture`만 실행해 실제 fd 입출력·exec 실패·backpressure·PID reap과 fake USB의 `CONNECT`+tail·suffix 보존·종료 예산을 검사합니다. sanitizer 빌드에서는 fixture도 ASan/UBSan을 적용합니다. 기존 주입 방식은 테스트 child의 `RDY` handshake 뒤 fake ready 이벤트를 넣습니다. 별도 `ppp_supervisor_start_observed()` 방식은 marker 없는 첫 binary stdout을 pending-TX에 원형 그대로 저장한 뒤 exec 완료와 함께 시작 gate로 사용합니다. 이 방식은 injected ready 우회를 거절하며 `begin_connect`를 USB read 전에 한 번만 호출합니다. 첫 출력은 설치본의 readiness·LCP·인증·IP 성공 증거가 아닙니다.

`src/ppp_usb.h/c`는 실제 interface182 비동기 IN·동기 OUT 어댑터입니다. `usb_observer.c`의 내부 owned session은 기존 신원·parent·전체 pipe 검증을 재사용하고 참조를 보유하며 설명자를 복사합니다. probe의 고정 source-pointer allowlist와 local copy는 그대로입니다. 어댑터는 생산 앱 링크 목록·CLI에 들어가지 않습니다. `make ppp-usb-sdk`는 실제 SDK로 컴파일만 하고, `ppp_usb_test`는 같은 구현에 fake SDK vtable/registry를 연결합니다. `verify-offline`은 전체 빌드·실제 SDK 컴파일·일반 테스트·sanitizer 테스트와 CLI·API 가드 검사를 함께 실행합니다.

### 단발 세션 실행 계층

`src/ppp_session.h/c`는 내부 opaque heap 객체로 metadata preflight → USB acquire → peer bind → 첫 step의 고정 observed start → supervisor step/stop → 정리를 묶습니다. production 앱 링크 목록·CLI·discovery에는 연결하지 않습니다. `make ppp-usb-sdk`에 실제 SDK compile-only 오브젝트를 추가했고, 동작 검사는 기존 fake SDK 번역 단위가 같은 구현을 include하도록 구성했습니다. 테스트에서만 고정 start 호출을 자체 child fixture의 observed start로 치환합니다. 2026-10-15 coordinator의 세 번째 전체 오프라인 검사(`build/ppp-session-20261015-3/verify.status`, `verify_exit=0`)에서 빌드·SDK 단독 컴파일·일반 15개·ASan/UBSan 15개 테스트와 CLI/API 가드가 통과했습니다. 모든 테스트는 `failures=0`이며 `ppp_usb_test`는 일반 167,485 checks, sanitizer 169,189 checks였습니다. 별도 검토에서 코드와 검사 로그를 확인했고 미해결 결함은 없었습니다.

- `ppp_session_start(request, metadata_backend, &session)`은 출력 핸들이 NULL이어야 합니다. 잘못된 입력·할당 실패는 -1이며 기존 live 출력은 덮어쓰지 않습니다. 0은 객체를 수락했다는 뜻이지 연결·실행 승인·정리 성공이 아닙니다. 수락 뒤 metadata/acquire/bind 실패도 오류 객체를 반환합니다.
- request의 service/device/config·timeout과 metadata backend는 start 안에서 소비합니다. budgets/capacity는 값으로 복사합니다. request timeout은 OUT 전용이며 NULL이면 기존 acquire의 1000ms 기본값입니다. IN timeout은 start가 CONNECT·SESSION·DRAIN 예산 합 + 1000ms로 계산해 acquire에 넘기고, 0이거나 uint32_t를 넘으면 -1로 거절합니다. NULL metadata backend는 기존 preflight의 실제 호스트 조회를 선택하므로 오프라인 테스트는 항상 fake metadata를 제공합니다. start의 동기 호출 사이에는 취소점이나 SDK 절대 시간 상한이 없습니다.
- 상태는 `PREPARED`, `RUNNING`, `DISPOSING`, `CLOSED`뿐입니다. 첫 step은 start만 한 번 호출하며 stdout/USB read는 다음 step부터입니다. 성공 뒤 내부 caller adapter/peer 별칭을 폐기하고 supervisor만 정리합니다. bind/start 실패는 실패한 supervisor를 읽거나 step하지 않고 caller dispose로 정리합니다.
- stop은 latch만 설정합니다. PREPARED stop의 다음 step은 spawn 없이 dispose로 갑니다. RUNNING stop의 다음 적법한 START/CONNECT/SESSION step은 supervisor stop만 한 번 전달합니다. 이미 DRAIN/CANCEL/CLEANUP·오류 경로면 중복 stop 없이 기존 정리를 진행합니다.
- step은 진행 중 0, 로컬 CLOSED에서 1, 잘못된 호출에서 -1입니다. 운영 오류는 typed snapshot의 최초 단계 오류와 유효한 supervisor phase/error로 따로 확인합니다. `cleanup_error`는 caller dispose 결과와 기존 supervisor가 노출하는 close 오류만 반영하며 모든 중간 정리 오류를 별도로 관측하지는 않습니다. 오류 후에도 CLOSED까지 step을 계속하고 WAIT에서는 해제하지 않습니다.
- snapshot을 포함한 모든 handle API는 생성한 pthread·CFRunLoop와 busy guard를 따릅니다. 잘못된 owner/loop·재진입은 상태와 출력 무변경으로 거절합니다. 객체 자체가 RunLoop 참조를 CLOSED 뒤 release까지 보유합니다. release는 CLOSED에서만 성공하고 같은 핸들을 NULL로 바꾸며 NULL 핸들 반복은 no-op입니다.
- `ppp_session_peer_done()`은 실제 supervisor DRAIN에만 명시적으로 전달합니다. EOF/reap/close로 합성하지 않습니다. 이를 입증할 상위 계층은 아직 없으므로 정상 LCP 종료·AT/PDP 복원으로 해석할 수 없습니다. raw adapter/supervisor getter나 ready 주입 API는 없습니다.

실제 USB open/송신·CGDATA·실제 pppd 실행·호스트 preflight·네트워크 변경은 하지 않았습니다. 아래 내부 실행기 외에 blocking owner loop·launchd·자동 재접속·PPP 인터페이스 귀속/복원은 추가하지 않았습니다. 설치본 인증과 readiness, 후손 전체 종료는 여전히 미검증입니다. 기존 SIGPIPE 실패의 원인은 미확정이며 후속 미재현을 원인 규명으로 해석하지 않습니다.

### 내부 slice 실행기와 신호 복원

`src/ppp_runner.h/c`는 같은 owner pthread·CFRunLoop에서 단발 세션을 소유하는 내부 실행기입니다. production 앱 링크·CLI·discovery와 기존 session/USB/supervisor/child generic ABI는 바꾸지 않습니다. 2026-10-16 개발자의 보완본 전체 오프라인 검사(`build/ppp-runner-20261016-2/verify.status`, `verify_exit=0`)에서 빌드·일반 15개·ASan/UBSan 15개 테스트와 CLI/API 가드가 통과했습니다. 모든 테스트는 `failures=0`이며 `ppp_usb_test`는 일반 176,373 checks, sanitizer 179,505 checks였습니다. checks는 입력 수나 coverage 수가 아닙니다. runner/session의 실제 SDK 단독 compile-only는 첫 검사 `build/ppp-runner-20261016/verify.stdout` 2–3행에서 성공했고, 테스트만 보완한 두 번째 검사는 해당 최신 오브젝트를 재사용했습니다. LOW 회귀 공백 보완 뒤 독립 코드·테스트·원 로그 검토에서 미해결 발견 사항은 없었습니다.

1. fake 또는 별도 승인된 metadata 경로로 세션을 준비한 뒤 `ppp_runner_begin(&session, &runner)`을 호출합니다. `*runner`는 NULL이어야 합니다. 입력·owner·재진입·할당 거절은 -1이며 두 핸들을 그대로 둡니다. 수락은 0이며 `session`을 NULL로 바꿉니다. signal 설치 실패도 runner가 소유하므로 원래 session 별칭을 다시 쓰지 말고 slice로 정리합니다. 이미 CLOSED인 세션은 handler 설치를 생략합니다.
2. `ppp_runner_slice(runner, stop_flag, wait_ms, &status)`를 반복합니다. stop_flag가 NULL이면 추가 요청이 없고 wait_ms가 NULL이면 본문에서 10ms를 고릅니다. 지정값은 1–50ms입니다. slice는 session step과 `nanosleep`을 각각 최대 한 번 수행하고 CLOSED이면 기다리지 않습니다. EINTR 뒤 remaining 재수면은 하지 않습니다. 외부 CFRunLoop dispatch는 추가하지 않습니다. 반환 0은 진행, 1은 로컬 CLOSED, -1은 출력 무변경 거절입니다.
3. `ppp_runner_restore(runner, &status)`로 저장된 action을 복원합니다. live 세션이면 먼저 stop을 latch하고 SIGTERM→SIGINT 역순으로 남은 항목을 모두 시도합니다. 반환 0은 복원 완료, 1은 미복원 항목이 남음, -1은 무변경 거절입니다. 실패한 항목만 재시도하며 성공한 복원을 다시 설치하지 않습니다. WAIT 중에도 복원할 수 있지만 이후 slice로 정리를 계속해야 합니다. 복원 후 새 signal은 기존 action을 따르므로 기본 종료를 포함하며 더는 graceful stop을 보장하지 않습니다.
4. 로컬 CLOSED와 `unrestored_actions == 0`을 모두 확인한 뒤 `ppp_runner_release(&runner)`로 해제합니다. 같은 소유 핸들이 NULL로 바뀌며 NULL 핸들 반복은 no-op입니다. live/미복원 상태의 release는 거절합니다. process-static lease는 restore가 아니라 최종 release에서만 끝나므로 다른 실행기는 그때까지 시작할 수 없습니다.

status는 session snapshot과 별개로 최초 실행기 오류, signal 번호, stop latch, 설치 항목과 현재 미복원 bitmask, SIGINT/SIGTERM별 최초 install/rollback/restore errno 및 wait errno를 보존합니다. 복원 재시도가 성공해도 과거 오류는 남습니다. 두 번째 설치 실패 때 첫 설치를 즉시 rollback하며 rollback 실패를 설치 실패와 별도로 기록합니다. 로컬 CLOSED·signal 복원·물리 장치/네트워크 복원 성공은 서로 다른 조건입니다.

호출자는 프로세스 전체 SIGINT/SIGTERM disposition 변경을 직렬화하고 observer 등 다른 소유자와 겹치지 않게 해야 합니다. 다른 thread는 두 signal을 차단하고 session owner가 받도록 준비해야 합니다. 실행기는 일반 mask API나 모든 전제 위반의 검출을 제공하지 않습니다. handler는 process-static `volatile sig_atomic_t`에 대입만 하며 세션·runner·stack 포인터, SDK·CFRunLoop·할당·로그를 쓰지 않습니다. handler action의 mask에는 두 signal을 넣고 flags는 0으로 SA_RESTART를 켜지 않습니다.

요청은 step 전·후와 대기 후에 확인합니다. 관측 전에 이미 시작한 동기 SDK/step이나 begin 이전의 session_start 동기 구간을 취소하지 못합니다. 신호 수신 즉시 모든 OUT을 막거나 대기/SDK의 hard wall-clock 상한을 보장하지 않습니다. 반복 요청은 stop 전달·기한·Abort·close·release를 늘리지 않습니다. callback이 남은 WAIT는 강제 free로 숨기지 않습니다. ready·peer_done·EOF/reap/quiet를 합성하지 않으며 SESSION 종료 교환을 입증할 상위 계층도 없습니다. 따라서 peer_done 없는 DRAIN은 timeout 오류와 로컬 정리만 검사 대상으로 둡니다.

새 회귀 검사는 기존 `ppp_usb_test.c` 번역 단위에서 같은 runner 구현을 include하고 fake sigaction/nanosleep을 해당 include에만 치환합니다. 별도 production 주입 ABI는 없습니다. 입력/할당/owner/재진입 보존, 설치·rollback·복원 실패와 재시도, saved action 보존, PREPARED/START/CONNECT/SESSION 요청 경계, slice당 step/sleep 상한, EINTR·wait 오류, 지연/inline callback과 WAIT 자원 보유, lease·참조 균형을 검사하도록 작성했습니다. 실제 SDK는 독립 compile-only 대상으로만 추가했습니다. 설치 중 SIGINT와 첫 SIGTERM을 후속 주입으로 덮지 않고 첫 slice에서 각각 관측하는 회귀를 보완한 뒤 전체 검사를 다시 통과했습니다. 두 번째 검사 stderr에서 FAIL·ASan/UBSan·runtime·compiler·make 오류 패턴은 없었고, invalid status 파일 65개는 모두 exit 2였으며 마지막 WritePipe 가드까지 완료했습니다. 이는 오프라인 검사 결과입니다. 실제 USB·pppd·설치본 인증/readiness·PPP 인터페이스 귀속/복원·후손 전체 종료·LCP·AT/PDP 복원은 미검증입니다.

### 하위 어댑터의 호출자 정리

acquire 이후 아직 호출자 소유인 어댑터는 `ppp_usb_dispose_step(&adapter)`로 단계별 정리합니다. 소유권은 호출자 계약이며 API가 start 성공 여부를 런타임에 판별하지 않습니다. 정리 API는 저장된 supervisor 포인터나 그 필드를 읽지 않습니다. bind만으로 소유권이 이전되지는 않습니다.

| 결과 | 어댑터 소유자와 다음 동작 |
| --- | --- |
| acquire 실패 | 반환 핸들은 NULL. 획득한 자원만 내부 정리하며 peer를 쓰지 않음 |
| acquire 성공, 아직 bind하지 않음 | 호출자가 dispose를 끝까지 진행 |
| bind 실패 또는 bind 성공 뒤 start 전 | 호출자 소유 유지. dispose 가능 |
| start 동기 실패(입력 검증·초기 clock·fork 전 child 인자 실패 등) | 호출자 소유 유지. 실패한 supervisor가 초기화됐다고 가정하지 않고 dispose |
| start 성공 | supervisor로 소유권 이전. 호출자 adapter/peer 별칭을 버리고 supervisor step/cleanup만 사용 |
| start 성공 뒤 비동기 exec `ENOENT` | supervisor 소유인 오류 세션. caller dispose 대상이 아니며 CLOSED까지 step |

`struct ppp_usb_dispose_result`의 `released`와 `error`는 독립입니다. 같은 owner thread·CFRunLoop에서 직렬 호출하며 signal handler에서 호출하지 않습니다.

| dispose 결과 | 호출자 동작 |
| --- | --- |
| `{0,0}` | WAIT. 같은 핸들과 모든 자원을 유지하고 callback 반환을 기다리며 다시 호출 |
| `{0,1}` | 오류를 별도로 보고하고 핸들 유지. owner/RunLoop·pumping·재진입 거절은 무변경이므로 올바른 호출 문맥에서 다시 호출. 정상 문맥의 WAIT 오류도 quiet 전 해제 금지 |
| `{1,0}` | 로컬 해제 완료, 핸들 NULL. 빌린 peer/context를 폐기 |
| `{1,1}` | 오류가 있는 로컬 해제 완료, 핸들 NULL. 빌린 peer/context를 폐기하고 반환 오류를 보존. 물리 close·장치·모뎀·네트워크 복원 성공이 아님 |

이중 포인터 자체가 NULL이면 `{0,1}`, 이미 NULL인 핸들이면 `{1,0}`입니다. 완료 호출의 오류는 그 반환값으로 소비해야 하며 후속 NULL no-op에는 과거 오류가 남지 않습니다. 정리를 시작하면 재bind·재start하지 않습니다. 반복 안전성은 NULL로 갱신되는 **같은 소유 핸들**에만 적용하며 복사한 stale 포인터로 재호출하면 안 됩니다. WAIT에서 Abort 실패·timeout을 quiet로 간주하거나 강제 free하지 않습니다. 별도 deadline이나 새 I/O는 추가하지 않습니다.

fake SDK 회귀 검사는 미bind·bind 실패·동기 start 실패, 지연 callback·Abort 오류·inline completion·재진입·wrong owner/RunLoop·close 오류와 NULL 반복을 다룹니다. 초기 clock 실패/overflow는 복사한 test peer의 fake `now_ms`로만 만듭니다. start 실패 뒤 pending 검사는 **test-only `peer.read`를 start 전에 직접 호출한 합성 상태**이며 production 시작 순서가 아닙니다. 이 정리 API는 실제 실행 허가·자동 연결·인터페이스 복원을 추가하지 않으며 `auth-policy=unverified`, `interface-cleanup=unverified`, `installed-source=unmatched`를 유지합니다.

START/CONNECT의 `stop()`은 요청 수락 시 0을 반환하고 `CANCEL`로 전환합니다. PPP 접속·종료 성공을 뜻하지 않습니다. 최초 요청에서 신규 begin_connect·USB read/write·readiness 전이를 막고, 기존 CONTROL_WAIT의 core stop이 정확히 INVALID/CONTROL/recovery-unknown으로 끝났을 때만 예상 취소로 구분합니다. 이미 보낸 CGDATA의 취소·복구를 주장하지 않습니다. 소유한 직접 child에 SIGTERM을 최대 한 번 요청하고 `drain_ms` 동안 reap·실제 pipe EOF·exec 오류 해소·peer quiet를 기다립니다. exit 0 또는 실제 TERM 송신 뒤 SIGTERM 종료만 예상 결과이며 exec/I/O/소유권 상실·다른 종료는 오류입니다. pre-exec 신호도 handshake EOF를 만들 수 있어 exec_done만으로 exec 성공을 단정하지 않습니다. SESSION의 raw 종료 교환 drain과 일반 오류의 즉시 SIGKILL 정책은 그대로입니다. 반복 stop은 기한·signal·quiesce 횟수를 늘리지 않습니다. 취소는 peer_done·drain_complete를 합성하지 않으며 진단에 cancellation과 core-error, recovery=unknown을 남깁니다. 로컬 취소도 LCP·AT/PDP 복원·인터페이스 정리 증거가 아닙니다.

cleanup은 신규 peer I/O를 막고 pending IN에 한 번만 Abort를 요청합니다. 정상 종료는 기존 drain 기한 안에서 큐 전달·stdin close·직접 자식 정상 회수·stdout/stderr 실제 EOF·exec handshake 해결·peer quiet를 모두 기다린 뒤 drain 완료를 확정합니다. 정상 cleanup에 새 기한을 주지 않습니다. 최초 오류 cleanup에서만 `drain_ms` 길이의 transport 기한을 정하고, 늦은 오류에도 별도 일회 전환과 아직 소유한 PID의 SIGKILL 요청을 수행합니다. stdout은 정상 읽기와 같은 step의 오류 cleanup 폐기를 합쳐 최대 4096바이트이며 stderr 폐기는 별도 step당 최대 4096바이트입니다. quiesce 이후 남은 미전달 성공 IN은 callback이 모두 반환했어도 오류로 반영해 drain 성공으로 유실하지 않습니다. 기한 뒤 남은 로컬 FD를 닫으면 `transport_abandoned`이며 EOF로 바꾸지 않습니다. `ECHILD`만 `ownership_lost`로 구분하고 정상 회수로 세지 않으며 이후 PID 신호를 금지합니다.

Abort 성공·실패·기한 경과·clock 오류는 자식 소유권 종료나 quiet 증거가 아닙니다. 직접 자식 정상 회수 또는 명시적 소유권 상실과 실제 callback 반환을 모두 확인한 뒤에만 core join → source 제거 → close → release를 수행합니다. 미회수 자식이나 callback이 남으면 WAIT와 소유권을 유지합니다. 오류 CLOSED는 성공이 아니라 로컬 자원 정리 완료입니다. 어느 CLOSED나 EOF도 후손 전체 종료 증거가 아니며 진단은 `descendants=unproven`을 고정합니다. 동기 OUT과 SDK timeout은 전체 wall-clock 상한이 아니며 callback dispatch를 늦출 수 있습니다. 실제 USB callback 보장이나 장치 수신량을 fake 검사·SDK write 성공으로 주장하지 않습니다.

`src/ppp_launch.h/c`는 실행하지 않는 정책 준비 API입니다. `/usr/sbin/pppd`, cwd `/`, 빈 환경 `{NULL}`과 고정 argv `notty nodetach nopersist noauth noipdefault nodefaultroute noipv6 nolog refuse-pap refuse-chap refuse-mschap refuse-mschap-v2 refuse-eap noacsp`를 제공합니다. `noacsp`는 2026-10-17에 더했습니다(아래 「다섯 번째 실기 1회 결과」). 다섯 `refuse-*` 옵션의 근거는 공개 pin의 [`auth.c`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/auth.c) 옵션 정의와 `auth_reset()`입니다. 설치본 1029의 지원·인증 식별값 미송신을 보장하지 않습니다. `auth-policy=unverified`, `interface-cleanup=unverified`, `installed-source=unmatched`를 유지합니다. 사용자 path/argv/env override, production 실행 CLI, 자동 sudo는 없습니다. `noauth`는 상대 인증 정책을 제어하며 로컬 인증을 요구하는 peer에서 PAP/CHAP을 자동 해결하지 않습니다. 이는 peer가 요구하는 인증이 실패한다는 보장이 아닙니다. `usepeerdns`도 요청하지 않습니다. metadata-only preflight는 고정 executable·options/postoptions·secrets·기본 IP/auth/IPv6 hook과 실제 UID의 `getpwuid(getuid())` home `.ppprc`를 확인합니다. `HOME`은 쓰지 않으며 내용이나 개인 경로를 출력하지 않습니다. known `/etc` 매핑은 `/private/etc`로 검사하고 다른 parent/final symlink·잘못된 타입·접근 실패·사용자 조회 실패를 구분합니다. 설정/hook/secrets가 존재하거나 불명확하면 차단합니다. 점검이 clear여도 실행 승인이 아니며 metadata→exec 경쟁 조건은 남습니다.

사용자는 공개 pin 기준으로 남는 실행 사용자·uid와 접속 뒤 IP·DNS 시스템 로그를 허용했습니다. `sensitive-log=unsuppressed`는 그 허용과 미억제이며 실행 허가가 아닙니다. `execution=unapproved`는 metadata clear 뒤에도 유지합니다. 진단의 `auth-policy=unverified`와 `interface-cleanup=unverified`는 보장할 수 없는 동작을 표시할 뿐 실행 gate가 아닙니다. 실행이 가능해지면 시작 직전에 `ppp_launch_preflight(NULL)`의 고정 label을 확인하며, present·access-error·symlink·wrong-type·user-error는 차단입니다. 이 수정에서는 호스트를 조회하거나 pppd를 실행하지 않았습니다.

사용자는 고정 정책 1회와 종료 뒤 PPP 인터페이스를 내리는 범위를 승인했지만, 종료 후 해당 인터페이스가 정확히 이 세션 소유인지와 내려갔는지를 검증할 구현이 없습니다. 따라서 약속한 정리 조건을 충족하지 못해 실제 실행은 보류합니다. 기본 경로·DNS 설치·NAT·공유·다른 인터페이스 변경은 범위 밖이며 인터페이스·주소·peer route는 생길 수 있습니다. 기존 `noauth`만의 정책에서는 공개 pin의 hostname 기본 사용자명과 기본 EAP 허용·협상 경로가 남았습니다. 새 다섯 `refuse-*` 옵션은 이 인증을 거부하도록 의도한 정책입니다. 다만 설치본 1029의 옵션 지원·동작은 확인하지 않았으므로 인증 식별값 미송신을 보장하지 않고 `auth-policy=unverified`로 남깁니다. 공개 소스의 코드 경로를 실제 인증 패킷 송신 관측으로 해석하지 않습니다. charshunt 세션 탈출은 미확인이며 직접 자식 종료는 후손 종료 증거가 아닙니다.

Apple ppp 검토 기준 revision은 `06a73c498c93551b7b40bf08424d437c0d3985e5`입니다. 순수 `notty` 경로는 시작 시 `TCSAFLUSH`·`ttyflush`와 shunt stdin/nonblocking 설정 뒤 첫 stdout을 내보냅니다. stdout이 로그·설정·plugin·script에 오염되지 않는 고정 경로에서만 첫 출력 이후 접속, CONNECT 뒤 stdin 공급 순서를 지지합니다. 이후 오류/종료 flush나 정상 동작까지 보장하지 않습니다. 설치본 `LC_SOURCE_VERSION=1029.0.0.0.1`과 검토한 소스 tag `ppp-1027.121.1`은 다릅니다. 서명·문자열만으로 동일성을 승인하지 않습니다. `nolog`는 fd 로깅만 끄고 별도 시스템 로그에는 실행 사용자·IP/DNS 기록 경로가 남습니다. 공개 pin의 실행 사용자·uid 및 IP/DNS 잔여 로그는 사용자가 허용했으며 이 구현은 억제하지 않습니다. 설치본의 로그 동작·옵션 지원·입력 readiness는 실기 미검증입니다. 이번 공식 GitHub 조회에서는 설치본 1029에 대응하는 소스를 찾지 못했습니다. argv 전 사용자 설정과 argv 뒤 postoptions, 접근하지 못한 root `.ppprc`도 실행 전 차단 조건으로 남습니다. `notty`의 별도 charshunt 때문에 직접 PID 회수만으로 통신 경로 종료를 판단하지 않습니다. process group/session 격리·음수 PID 신호·WNOWAIT·group-ready handshake는 구현하지 않았고 charshunt의 세션 탈출 여부도 미확인입니다. 공개 pin의 `main.c`에는 정상 종료 시 `die`에서 cleanup을 거쳐 `sys_cleanup`과 `disestablish_ppp`로 가는 경로가 있습니다. 그러나 kernel이 마지막 socket close 때 인터페이스를 내리는 동작과 설치본 1029의 대응은 확인하지 않았으므로 모든 종료 상황에서 인터페이스가 내려간다고 단정하지 않습니다.

공개 핀 `06a73c498c93551b7b40bf08424d437c0d3985e5` 소스 판정: `utils.c`의 `log_write`는 `sys_log`를 먼저 호출하며, `log_to_fd`가 음수일 때만 fd 쓰기를 건너뜁니다. `sys-MacOSX.c`의 `sys_log`는 `vsyslog`를 사용하고, NetworkExtension controller가 활성이면 `os_log_with_type`을 사용하며 `nolog`를 참조하지 않습니다. `main.c`는 시작 시 `"started by %s, uid %d"` 형식으로 실행 사용자와 UID를 로그합니다. `ipcp.c`는 `notice()`로 로컬·원격 IP와 기본·보조 DNS를 억제 옵션 없이 기록합니다. 설치본 `LC_SOURCE_VERSION 1029.0.0.0.1`은 소스 tag `ppp-1027.121.1`과 다르며, 1029에 대응하는 소스는 확인하지 못했습니다. 이 연결은 실행 허가, 설치 바이너리 동일성 확인, 민감 로그 억제, TOCTOU 수정을 뜻하지 않습니다. 사용자의 고정 1회 승인은 받았지만 인증 정책과 인터페이스 정리 조건을 충족하지 못해 실제 실행은 보류합니다.

`ppp_usb.c` 내부 `begin_connect`는 바인딩·owner·CONNECT phase·ready·exec 완료·no-error·미접속 gate에서만 고정 `AT+CGDATA="PPP",1\r`를 한 번 요청합니다. SDK 호출 전에 attempt를 고정하며 기존 `usb_session_write()`와 단일 SDK writer를 재사용합니다. hook 실패·늦은 성공·clock 실패 뒤 새 read/재송신은 없고 pending callback pump는 정리까지 유지합니다. probe allowlist는 그대로입니다. 이 구현과 fake 검사는 실제 장치 송신이나 pppd 실행 허가가 아닙니다.

실제 USB 전송, `/usr/sbin/pppd`·PTY·스레드 실행, PPP 협상·인증·IP 설정과 AT/PDP 복원은 실행하거나 검증하지 않았습니다. EOF·child exit·빈 큐는 LCP 종료나 모뎀 복원 증거가 아닙니다. raw stdout은 byte core로만 전달하고 child stderr는 제한된 임시 버퍼에서 읽어 버립니다. 진단에는 고정 label·카운터·종료 코드만 남깁니다. API와 호출자 책임은 [PPP 설계 문서](docs/ppp-design.md#실제-childpipe와-injected-peer-계약)에 정리했습니다.

컴파일은 `-Wall -Wextra -Werror`를 적용합니다. sanitizer는 장치 없는 파서·관찰 정책·수신 분석·재연결 대기·AT 송신 정책 단위 테스트에 AddressSanitizer/UndefinedBehaviorSanitizer를 적용합니다. `verify-offline`은 기본 장치 조회나 실제 `--observe`·재연결 대기·`--probe-at`·`--probe-ati`를 실행하지 않습니다. 송신 모드는 장치 접근 전에 끝나는 인자 오류 경로(종료 2)만 실행합니다. 잘못된 `--observe invalid` 인자는 장치 접근 전에 종료 2로 거부합니다.

테스트는 정상 209바이트 fixture와 모든 잘린 prefix, 헤더/totalLength 불일치, 모든 설명자의 길이 0/1/과대 값, 4096바이트 경계, 고유 인터페이스·endpoint 개수 불일치, 중복 인터페이스/alternate·endpoint, 여러 alternate, 알 수 없는 설명자 보존, endpoint 순서, ASCII 파일 경계를 검사합니다.

관찰 정책 테스트는 fake transport를 쓰는 오프라인 단위 테스트입니다. 실제 장치 성공을 대신하지 않습니다. 기본 CLI가 관찰 함수를 호출하지 않는지, 정확한 설명자·pipe 일치, 2초/15초 deadline·64KiB·8회 상한, 오류 바이트 무시, timeout 이후 재읽기 없음, 취소·분리·busy·권한·미지원·close 실패 때 정리를 검사합니다.

빌드·테스트 종료 코드와 stdout/stderr, 기본 캐시 실행 결과는 `build/`에 저장하며 git에서 제외합니다. `make test` 같은 수동 명령은 결과를 터미널에 출력합니다. 제출된 실행 기록은 `compile.*`, `tests.*`, `sanitize.*`, `offline-cli.*`, `cached.*`입니다.

## PPP 로컬 메타데이터 점검

생산 앱과 별개인 수동 점검 명령입니다. 빌드에는 위 메모리·공용 잠금 규칙을 적용하세요.

```sh
make build/ppp_local_check
./build/ppp_local_check --help
./build/ppp_local_check --check-host
```

`--help`와 `--check-host`는 각각 단독으로만 받습니다. 무인자·중복·다른 인자(`--run`, `--version`, 경로/옵션 override 포함)는 호스트 접근 전에 거절합니다. 종료 코드는 도움말 0, 내부/출력 오류 1, 인자 오류 2, 점검 후 실행 차단 3입니다. 이 명령의 3은 예상한 차단 결과이지 설치된 `pppd`의 종료 코드나 PPP 협상 결과가 아닙니다.

`--check-host`는 UID/EUID/GID/EGID 중 하나라도 0이면 `identity=root`로 중단합니다. UID root뿐 아니라 root group(GID/EGID 0)도 보수적으로 차단하는 분류입니다. UID와 EUID 또는 GID와 EGID가 다르면 `identity=elevated`로 중단합니다. 고정 실행 파일과 부모의 symlink·타입·root 소유·group/other 쓰기 금지, 실행 파일의 실행 비트·non-setuid/non-setgid metadata를 확인한 뒤 기존 preflight로 옵션·인증·hook 파일의 부재를 확인합니다. 파일 내용·개인 UID·계정명·home 경로는 출력하지 않습니다. 기존 `policy` 진단의 `execution=unapproved`는 그대로이고, 마지막 `local` 줄이 이 명령의 최종 판정입니다. 확인 실패나 생략은 clear가 아닙니다. nonroot/non-setuid 조건에서는 `privilege=unavailable`로 분류하고, 실행 파일 조건을 확인하지 못하면 `privilege=not-checked`로 남깁니다.

metadata가 clear여도 `execution=blocked system-impact=unproven installed-source=unmatched`입니다. 자체 pipe 상대와 `notty`만으로 커널 PPP·SystemConfiguration 영향을 격리할 근거가 없고, `dryrun`도 옵션 파일 처리와 시스템 검사 경로를 거칩니다. 따라서 설치 `pppd`는 실행하지 않습니다. USB·LTE·sudo·설정 변경·시스템 로그 조회도 하지 않습니다. 이 점검은 TOCTOU 해결, 설치본 옵션/인증 지원 또는 인터페이스 정리의 증거가 아닙니다. 실제 접속 단계는 권한·허용할 시스템 영향·정리 확인 방법을 정하고 별도 승인을 받아야 합니다.

`make test`, `make test-sanitize`, `make verify-offline`에는 같은 checker 구현의 fake-only 검사를 연결했습니다. NULL 기본 경로도 테스트 번역 단위 안에서 치환하므로 실제 호스트 점검은 하지 않습니다. `verify-offline`의 실제 checker 실행은 도움말과 잘못된 인자만이며 `--check-host`는 수동 실행으로 남깁니다.

2026-10-16 개발자의 `build/ppp-local-check-20261016/` 검사 결과는 `verify_exit=0`입니다. 일반 16개·ASan/UBSan 16개 테스트 모두 `failures=0`이며 checker는 각각 3,018 checks였습니다. SDK compile-only 타깃은 기존 최신 오브젝트를 재사용했으며 이번에 재컴파일하지 않았습니다. CLI/API 가드를 마쳤고 기존 잘못된 인자 65건과 checker 1건은 모두 exit 2였습니다.
개발자가 별도로 `--check-host`를 한 번 실행한 결과는 rc 3, stderr 빈 파일입니다. `policy metadata=clear executable=present`이고 options/postoptions/user-options·PAP/CHAP/SRP·모든 hook은 absent였습니다. 최종 결과는 `local metadata=clear identity=nonroot executable=clear preflight=clear privilege=unavailable execution=blocked system-impact=unproven installed-source=unmatched`이며 인증 정책과 인터페이스 정리는 unverified입니다. 점검 시점의 metadata 관측일 뿐 TOCTOU 해결·시스템 무영향·PPP 협상 증거가 아닙니다. 실제 `pppd`·USB·LTE·sudo·시스템 변경은 실행하지 않았습니다.

## 종료 코드

| 코드 | 의미 |
|---|---|
| 0 | 캐시 또는 fixture 검증 완료. LTE·프로토콜 식별 성공을 뜻하지 않습니다. |
| 2 | CLI 인자 오류 |
| 3 | 대상 장치 없음 |
| 4 | 같은 VID/PID 대상이 여러 개 있음 |
| 5 | 장치 분리 또는 식별 재확인 실패 |
| 6 | 접근 권한 오류 |
| 7 | 점유 충돌 |
| 8 | accessor/API 또는 필수 registry 메타데이터 미지원 |
| 9 | 설명자/fixture 검증 실패 |
| 10 | 그 밖의 시스템 오류 |
| 11 | SIGINT/SIGTERM 취소 |
| 12 | 열린 인터페이스 close 실패. 앞선 오류보다 우선하며 원래 IOReturn도 stderr에 남깁니다. |
| 13 | 재연결 대기 시간 안에 새 registry ID의 단일 대상이 나타나지 않음 |

장치가 실행 중 분리되면 일부 메타데이터가 출력된 뒤 실패할 수 있습니다. 종료 코드를 확인하세요. 접근 오류 때 자동으로 관리자 권한이나 드라이버 분리를 시도하지 않습니다.

## 관찰 접근 경계

공식 [libusb v1.0.30 Darwin 소스](https://github.com/libusb/libusb/blob/v1.0.30/libusb/os/darwin_usb.c)를 검토했습니다.

- `darwin_open` 1586~1637행에서 **1597행 `USBDeviceOpenSeize`**를 호출합니다. non-seize 요구를 충족하지 않아 사용할 수 없습니다.
- `darwin_claim_interface` 1836~1945행에서 인터페이스를 못 찾으면 **1859행 `darwin_set_configuration`**으로 구성을 바꿀 수 있습니다.
- [`core.c` 1854~1862행](https://github.com/libusb/libusb/blob/v1.0.30/libusb/core.c#L1854-L1862)은 release API가 SET_INTERFACE를 보낼 수 있다고 설명합니다. Darwin 구현의 단순 close 동작만 보고 범용 API 보장을 가정하지 않습니다.

`--observe`는 설치 SDK의 legacy `IOUSBInterfaceInterface182`와 **`USBInterfaceOpen` (seize 아님)** 계약을 사용합니다. `IOUSBLib.h` 2179~2196행은 temporary exclusive task 접근과 endpoint pipe 생성을 설명합니다. 이는 수동 버스 캡처가 아닙니다. 정상 열기도 잠시 인터페이스를 점유하고 pipe를 만들며, BULK IN 읽기는 동글의 수신 큐를 소비합니다. OS 내부에서 어떤 신호나 상태도 변하지 않는다는 절대 보장은 하지 않습니다. 현재 macOS 내부 구현 전체를 공개 소스로 입증한 것도 아닙니다.

단일 검색에서 retained service와 registry ID를 유지합니다. VID/PID/release 및 활성 구성 1, 정확한 209바이트 설명자, 인터페이스 0~4/alt 0을 검사합니다. 모든 인터페이스 서비스를 한 번 수집하고, 각 열기 전에는 child가 없는지 확인합니다. plugin 생성 뒤에는 그 구간에서 나타난 단일 식별 USB user-client child만 허용합니다. 다른 child, 알 수 없는 메타데이터, 드라이버 점유, busy/access/미지원은 즉시 중단합니다. 자동 driver detach나 우회 fallback은 없습니다. child 검사는 registry 시점 관측이지 드라이버 부착 경합을 원자적으로 배제하는 증명은 아닙니다. 정상 exclusive open이 점유 충돌을 판정합니다.

열기 전후와 읽기 전후에는 device/interface registry ID와 parent, registry config/alt/인터페이스 번호, 캐시 accessor 값과 endpoint 개수를 다시 확인합니다. `GetConfigurationValue`/`GetAlternateSetting`은 사용자 공간 캐시일 수 있으므로 실장치 GET_CONFIGURATION/GET_INTERFACE 응답이라고 주장하지 않습니다. 매번 읽는 registry 상태와 열린 뒤 kernel `GetPipeProperties`의 방향·번호·종류·packet·interval을 설명자와 대조하는 범위입니다. pipe는 열린 뒤에만 존재하므로 열기 전 검사는 설명자와 캐시/registry에 한정됩니다. 불일치나 분리면 close 후 중단합니다. OUT/interrupt endpoint는 메타데이터만 검사하고 읽지 않습니다.

관찰 정책은 monotonic clock 기준 인터페이스당 2초(준비 포함), 전체 읽기 단계 15초·64KiB, 인터페이스마다 최대 8회입니다. 각 전송의 no-data/completion timeout을 남은 deadline으로 제한하고 0ms는 보내지 않습니다. 초기 검색/열거와 동기 kernel open·메타데이터·close에는 강제 취소 timeout이 없으므로 프로세스가 반드시 15초 안에 끝난다고 보장하지 않습니다. SDK `ReadPipeTO` 2672~2699행은 BULK IN 전용이고 interrupt timeout은 미지원입니다. 오류 때 buffer/size는 무시합니다. timeout 뒤에는 해당 pipe를 재읽지 않고 close하며 clear-halt/reset을 하지 않습니다. SIGINT/SIGTERM 핸들러는 flag만 설정합니다. 진행 중 동기 읽기는 timeout 뒤 flag를 확인하고 정리합니다. timeout/수신 없음은 지원 여부나 프로토콜 판정의 근거가 아닙니다. payload·hex·USB 문자열·IMEI·네트워크 패킷은 출력하지 않습니다.

실제 장치 관찰은 구현의 독립 안전성 검토 뒤 coordinator가 실행합니다. 자동 드라이버 분리, reset, SetConfiguration/alternate, clear-halt, seize, USB control/vendor 요청, fuzz, 커널 확장·펌웨어 접근은 범위 밖입니다. USB OUT 송신은 앞의 「AT 1회 송신」·「ATI 1회 송신」·「상태 조회」에 설명한 `--probe-at`의 고정 `AT\r`, `--probe-ati`의 고정 `ATI\r`, `--probe-status`의 고정 조회 명령 4개(각 1회), 「데이터 연결 설정 조회」의 `--probe-pdp` 고정 `AT+CGDCONT?` 1회, 「데이터 방식 지원 조회」의 `--probe-data-support` 고정 `AT+CGDATA=?` 1회만 예외이며, 다른 AT 명령이나 다른 인터페이스 송신은 범위 밖입니다. SIM/APN/LTE 연결, 시스템 라우팅·DNS·NAT·공유 설정 변경도 별도 단계와 승인이 필요합니다. 기본 경로·DNS 변경은 `build/ppp_lte_up --up`만 예외로 의도하며, 그 실행도 매번 별도 승인을 받습니다. NAT·공유는 어느 실행기에도 없습니다.
