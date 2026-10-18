# USB bulk와 PPP 연결 설계

> 연결 설계와 오프라인 API 계약 문서입니다. 순수 C 전송 core, 로컬 child/pipe, supervisor, SDK IF2 어댑터와 별도 LTE 단발 실행기를 구현합니다. 생산 앱에는 실행 모드를 추가하지 않았습니다. 2026-10-17 사용자가 이전 커밋 `7e50263`의 실행기를 실기에서 한 번 실행했고, 앱 payload 송수신은 0/0이었으며 원인은 확정하지 못했습니다. 그 뒤 고친 현재 수정본은 실기에서 실행하지 않았습니다.

## LTE 단발 실행기의 좁은 연결

- `ppp_trial_preflight.[ch]`는 기존 auth trial의 root identity/executable/checked home/고정 `/var` alias와 진단을 추출합니다. 검사한 home/backend context는 session start가 끝날 때까지 유지합니다. 공용 launch NULL 정책·고정 항목·argv/빈 환경/cwd와 child ABI는 바꾸지 않습니다. 기존 auth fake TU는 추출 구현도 포함해 실제 공용 checker에 fake 경계를 연결합니다.
- `discovery_internal.h`의 synchronous with-one-target callback은 기존 단일 장치 선택/캐시 파싱/policy/registry ID·VID/PID 재확인을 재사용합니다. callback 진입에서는 registry ID·설명자 원문을 출력하지 않습니다. service/device/config는 callback 동안 borrow이고, callback은 runner CLOSED·release 전에 반환하지 않습니다. USB 점유·CGDATA는 기존 `ppp_usb`만 담당합니다.
- session/runner observation은 PID/start/reap/ownership/EOF/TERM/KILL/quiet 복사값만 반환합니다. fd/child/session 포인터를 반환하지 않습니다. 첫 start slice는 poll 없이 반환하고 그 직후 observer child identity를 한 번 캡처합니다. consumed session handle은 다시 쓰지 않습니다. runner observation은 pending signal도 먼저 반영하며 owner/RunLoop/reentry 계약을 따릅니다.
- observer의 historical `attributed`와 current validity는 다릅니다. 매 sample에서 동일 살아 있는 child identity, 현재 서비스 유일성/동일 key, IF 이름·index 존재를 재검사합니다. 소멸·조회실패·index 변경은 current copy 실패입니다. reap/ownership loss 뒤 PID를 재조회하지 않습니다. finish는 retained service key·IF 제거만 확인합니다.
- `ppp_lte_network.[ch]`는 단발 고정 echo 전용입니다. point-to-point IPv4 local/peer 유일성과 local-self 차단, 512B 이내 read-only scoped RTM_GET, type/version/msglen/PID/seq/index/error/up/reject/blackhole 및 sockaddr 길이/4바이트 정렬/bitmask bounds를 검증합니다. 형식이 모호하거나 지원하지 않는 응답은 fail-closed입니다. route mutation·fallback은 없습니다. 각 socket stage 앞 fresh 질의를 요구하며 전체 route 불변을 주장하지 않습니다.
- TCP는 `O_NONBLOCK`, `SO_NOSIGPIPE`, `IP_BOUND_IF` set/get index 확인 뒤 connect 한 번만 합니다. completion은 zero-timeout poll과 `SO_ERROR`로 확인합니다. EINTR/EAGAIN·부분 I/O는 다음 slice로 넘깁니다. 각 새 질의/connect/send 앞 pending stop/child/current IF/deadline을 확인합니다. IP_BOUND_IF는 no-leak 절대 보장으로 설명하지 않습니다.
- payload는 정확히 `dji-lte-once\r\n`, 길이는 `sizeof(PPP_LTE_PROBE)-1`입니다. 송신 완료 후 SHUT_WR, 동일 바이트 전부와 EOF만 app success입니다. 추가 바이트·잘림·다름·EOF 없는 timeout은 실패입니다. socket은 성공/중단/오류/shutdown 실패 모두 한 번 닫습니다. 임의 payload/기본 endpoint/DNS/proxy/redirect/retry는 없습니다.
- `ppp_lte_once.c`는 단일 엄격 CLI, 공유 preflight→observer baseline→borrow callback→session→runner→첫 시작 capture→IP/route/TCP→socket close→stop/정리→CLOSED→handler restore/release→observer finish를 연결합니다. 전체 60초/IP30초/TCP10초 목표, 시작5초/CONNECT10초/DRAIN3초입니다. 동기 호출과 미반환 callback은 절대 종료시한 밖입니다. callback WAIT를 조기 free/return하지 않습니다.
- `ppp_lte_once` 진단의 `phase-reached`는 slice 경계에서 관측한 오류 전 최고 phase라 한 slice 안의 전이를 놓칠 수 있습니다. `error-ms`는 첫 supervisor 오류를 관측할 때 따로 읽은 시각이며 stop·budget·clock 판정에 쓰지 않습니다. 시작 시각 부재, 조회 실패, 시작·직전 live 시각보다 역행이면 `unproven`입니다. 관측이 없거나 마지막 observe가 실패하면 새 진단 필드는 `none`/`no`/`0`이 아닌 `unproven`입니다. 다만 앞선 유효 관측에서 supervisor 오류를 이미 봤다면 `phase-reached`와 `error-ms`는 그 관측값을 유지합니다. `exec=ended`는 exec pipe가 오류 보고 없이 닫혔다는 뜻일 뿐 exec 성공 증거가 아닙니다.
- 원격 종료 검증은 구현되지 않았습니다. `peer_done`은 caller assertion이므로 사용하지 않습니다. 응답 뒤 TERM/DRAIN timeout/필요시 소유 직접 child KILL/reap/EOF/quiet를 보존하고 전체 `overall=partial`, exit 3입니다. 앱 echo·로컬 CLOSED·직접 reap·EOF·quiet·서비스/IF 제거·SC globals는 독립 증거입니다. 후손/PDP 복구 성공을 주장하지 않습니다.
- 새 network/once 테스트는 모든 native OS 경계를 TU fake로 대체합니다. 기존 USB 테스트에는 자체 child fixture를 쓰는 copy observation 회귀를 연결합니다. 일반/ASan·UBSan/SDK compile/help-invalid를 `verify-offline`에 연결합니다. native valid CLI는 자동 실행하지 않습니다. `ppp_lte_once_integration_test`는 실제 once와 실제 network 상태 기계를 한 TU에서 묶고 socket/route/ifaddrs 경계만 fake로 둡니다. 정상 echo, connect 뒤 IF index 변경 시 송신 0회, 수신 중 signal 뒤 추가 수신 없음, 다른 index route 응답 시 connect 0회, 주소 미출현 IP 30초 timeout 시 connect 0회를 확인합니다. 2026-10-17 최종 `verify-offline`(r7)은 일반·sanitize 모두 exit 0, 실패 0입니다(offline_stream 1,543,644, lte-network 10,930, lte-once 1,627, integration 1,575, auth trial 18,917회, ppp_usb 실패 0. ppp_usb 검사 횟수는 child fixture 타이밍에 따라 실행마다 조금 다릅니다).
- AF_ROUTE 소켓은 다른 송신자와 커널의 broadcast도 받습니다. 이런 메시지는 `rt_msghdr`보다 짧을 수 있습니다(`ifa_msghdr` 20B, `ifma_msghdr` 16B). 그래서 공통 앞 4바이트만 먼저 요구하고, type이 RTM_GET이 아니거나 pid·seq가 자기 요청과 다르면 건너뜁니다. step 한 번에 최대 8개까지 건너뛰고, 그 뒤에는 IP/앱 deadline 안에서 다음 step을 기다립니다. 자기 응답의 내용이 틀릴 때만 실패합니다. network 실패 사유는 처음 원인(예: `interface-changed`)을 유지합니다.
- runner 거부 뒤 session CLOSED 대기, slice 오류 뒤 정리 대기, restore 미완료 대기에는 상한이 없습니다. 빌린 장치를 runner 정리 전에 해제하지 않기 위한 의도적인 선택이며, 실제 실행에서 멈추면 출력 없이 대기할 수 있습니다. runner release 실패는 CLOSED·복원 완료 뒤에만 가능하며 재시도하지 않고 `cleanup=release-error`로 보고합니다.
- gate가 실패하면 signal·clock 이상을 먼저 `child-or-clock-unproven`으로 구분하고, 그 다음에 인터페이스 미입증을 판정합니다. 출력의 `session-error`(정상 정리 시 `supervisor-timeout`), `cleanup`(release/runner/session 정리 오류), `clock`은 각각 독립 필드입니다.

## 설치 PPP 인증 단발 시험의 한정 예외

`ppp_auth_probe.h/c`, `ppp_auth_trial.c`, `ppp_trial_observer.h/c`는 생산 브리지와 연결하지 않는 별도 `build/ppp_auth_trial --local-auth-once` 전용입니다. 실행 승인·영향·정리 경계와 정확한 단발 sudo 명령은 README의 「동글 없는 설치 PPP 인증 단발 시험」에 있습니다. 아래 기존 설치본 실행 보류 기록은 metadata-only 단계의 기록이며 이번 예외가 USB/runner/LTE에 적용되지는 않습니다.

2026-10-16 기존 구현의 최종 오프라인 gate는 `build/ppp-auth-trial-20261016-r5/`에 기록됐고 exit 0입니다. 일반 19개·ASan/UBSan 19개, 총 38개 suite summary 모두 `failures=0`입니다. 새 probe 8,139회·trial 7,058회·observer 396회 검사가 각각 양쪽에서 통과했습니다. 잘못된 CLI 인자 67건의 status는 모두 exit 2이며 SDK/production 컴파일은 r1–r5 증분 빌드에서 수행했습니다. stderr 3,346줄은 오류 패턴 검색 일치가 없었고 전문 독해를 뜻하지 않습니다.

별도 검토 뒤 개발자가 승인된 정확한 sudo 명령을 한 번 호출했으나 `sudo: a password is required`로 차단됐습니다. `local_auth_exit=1`, stdout은 비어 있고 기록은 `build/ppp-auth-trial-20261016-r5/local-auth.*`입니다. `reviewed.sha256`의 10개 항목은 호출 전후 모두 OK였습니다. trial/설치 pppd는 실행되지 않아 root preflight·SC 상태·실제 인증 거부·child/transport/인터페이스 정리는 미관측·미검증입니다. USB·LTE는 실행하지 않았고 재시도나 sudo cache 조작도 하지 않았습니다. 오프라인 통과나 sudo 호출을 실제 인증/정리 성공으로 해석하지 않습니다.

이후 사용자가 직접 실행해 보고한 `execution=blocked gate=root-home-preflight auth-reject=unverified child-cleanup=not-started`, exit 3은 위 sudo 차단과 별도입니다. 사용자 실행에서도 설치 pppd는 시작되지 않았습니다. 기존 출력은 home 검사와 공용 preflight 실패를 합쳐 보여 원인은 미확정이며 `/var` 시스템 링크는 호스트 metadata로 확인하지 않은 가설입니다. 이번 home/alias/진단 수정의 최종 오프라인 gate는 개발자가 `build/ppp-home-preflight-20261016-r2/verify.status`의 `verify_exit=0`으로 확인했습니다. 일반 19개·ASan/UBSan 19개, 총 38개 suite summary 모두 `failures=0`이며 trial 18,917회·probe 8,139회·observer 396회 검사가 각각 양쪽에서 통과했습니다. r1의 SDK/production 컴파일은 통과했지만 테스트 `snprintf` 매크로 충돌로 gate가 멈췄고, `#undef snprintf` 최소 수정 뒤 r2가 통과했습니다. r2 stderr 3,346줄의 오류·FAIL·runtime·sanitizer·make 패턴 검색은 일치가 없었으며 전문 독해가 아닙니다. 별도 정적 검토는 `APPROVE`, 발견한 실제 결함 0건입니다. 실제 재시험은 하지 않았습니다. 수정·오프라인 검증 중 host root home/설정 조회·root 실행·sudo·설치 pppd·USB·LTE·시스템 변경은 하지 않았습니다. 이 검증은 사용자 보고 차단 원인이나 실제 인증/정리의 증거가 아닙니다.

- framing: RFC 1662 초기 ACCM `0xffffffff`, flag/escape, FCS16 init `0xffff`/residue `0xf0b8`, 4096바이트 증분 frame입니다. exec 성공과 유효 LCP Configure-Request 전에는 요청하지 않습니다. PAP `03 04 c0 23`, CHAP-MD5 `03 05 c2 23 05`, MSCHAP `03 05 c2 23 80`, MSCHAPv2 `03 05 c2 23 81`, EAP `03 04 c2 27`을 고정 ID로 순서대로 각 한 번 보냅니다. 정확한 reject 뒤에만 다음 요청을 만듭니다. malformed 입력은 합성 검사 전용입니다.
- peer 제한: daemon의 정상 Configure-Request 재전송은 수신만 합니다. RFC 1661 §5.1의 MUST reply를 의도적으로 구현하지 않아 완전한 peer가 아닙니다. LCP Opened/authentication/IPCP/IP 진입이나 실제 인증값 제공이 목적이 아닙니다.
- I/O: stdin/stdout/stderr 각 누적 64KiB를 정리 중에도 유지합니다. 각 slice는 poll 실제 stderr 증가량을 4096바이트에서 먼저 빼고 남은 만큼만 read/write합니다. 다음 poll의 stderr 4096바이트를 예약할 수 없으면 `stderr-reserve-limit`으로 stop하고 이후 `ppp_child_poll_status()`/signal만 사용합니다. EOF 미관측은 남깁니다.
- 수명: child 시작 전 SIGINT/SIGTERM handler를 설치하고 flag만 기록합니다. monotonic 목표 20초, TERM 3초, KILL 뒤 2초이며 각 signal은 직접 소유 child에 최대 한 번입니다. ECHILD 뒤에는 signal하지 않습니다. 시계 실패/역행은 트래픽을 중단하고 제한된 status-only 관측 뒤 반환합니다. unresolved는 `ppp_child_close()`를 호출해 성공 처리하지 않으며 transport-abandoned/비성공입니다. 프로세스 반환/종료 뒤 fd/child 소유 유지나 후손 전체 정리는 보장하지 않습니다.
- metadata: UID/EUID=0과 GID/EGID 일치, 설치 실행 파일 및 `/`·`/usr`·`/usr/sbin`의 root 소유·쓰기 제한·형식을 요구합니다. 이번 trial 전용 수정은 `/`와 root passwd home의 물리 경로 전체에 존재하는 non-symlink 디렉터리·root 소유·group/other 쓰기 금지를 추가합니다. 기존 home 검사는 소유권·쓰기 제한을 보장하지 않았습니다. `/var` 또는 `/var/…` 경계의 root 소유 symlink만 예외로 두며 readlink 반환 길이와 정확한 `private/var` 또는 `/private/var` 대상만 비교해 고정 `/private/var`로 매핑합니다. symlink mode를 디렉터리 쓰기 권한으로 검사하지 않습니다. 일반 `realpath()`·다른 alias·`.`/`..`·빈 구성요소·비절대 경로·길이 상한 초과는 차단합니다. 검사한 동일 home을 기존 `ppp_metadata_backend.home`으로 실제 `ppp_launch_preflight()`에 전달해 `.ppprc`도 같은 물리 경로에서 검사합니다. real UID/lstat와 모든 고정 시스템 항목 검사, 공용 NULL 기본 선택·symlink 차단 정책은 유지합니다. 사용자 소유 시험 빌드를 root 소유 조건으로 막지 않습니다. 설정/인증 내용은 읽지 않으며 metadata 검사와 pppd의 원래 passwd home 사용 사이 TOCTOU·경로 동일성·무결성 한계는 남습니다.
- 차단 진단: `gate=root-home-preflight` 아래 고정 `reason`으로 home 조회/형식, alias metadata/type/owner/readlink/target, home metadata/type/owner-mode, 공용 preflight 실패를 구분합니다. `preflight=complete`에서만 고정 항목별 상태를 출력하고 선행 실패는 `not-run`, 공용 호출 실패는 `error`, 상태 범위/clear 불일치는 `invalid`로 구분합니다. 초기 zero 항목을 absent 관측으로 쓰지 않습니다. `auth-reject=unverified child-cleanup=not-started`·exit 3을 유지하고 개인 경로·UID·raw errno·링크 원문은 출력하지 않습니다.
- 관측: read-only SystemConfiguration snapshot과 `proc_pidinfo(PROC_PIDTBSDINFO)` 시작 식별자는 첫 child poll/reap 전에 확보합니다. alive/직접 소유 중에만 시작 식별자 일치를 조회합니다. 시작 전 없는 유일한 PPP service key(새 key는 `pid` 유무와 관계없이 모두 셉니다)와 고유 `InterfaceName`, 실제 존재한 커널 인터페이스가 결합돼야 귀속합니다. 그 service의 `pid`는 없으면 통과, 숫자이고 child PID와 같으면 통과, 다르거나 숫자가 아니면 후보가 아닙니다. 시작 전부터 있던 key는 세지 않되 현재 `pid`가 child PID와 같으면 PID 재사용 위험으로 ambiguous입니다. 새 key 둘 이상·이미 귀속한 key와 다른 새 key·`InterfaceName` 중복이나 시작 전 존재·누락 snapshot은 unproven입니다. 종료 후에는 retained service key의 삭제와 `if_nametoindex()` 부재를 따로 확인합니다. 숫자 PID를 다시 사용하지 않습니다.
- snapshot 범위: `State:/Network/Global/IPv4`, `IPv6`, `DNS` 내부 비교만 changed/unchanged/unknown으로 출력합니다. 커널 routing table 전체의 불변이 아닙니다. service key·주소·인증 원문·UID·개인 경로는 출력하지 않습니다. 실제 변경/잔여는 다음 실행 차단이며 관측 한계는 후속 LTE 안에서 별도 위험 판단/승인합니다. route 삭제·ifconfig down·Dynamic Store 삭제 같은 복구기는 없습니다.
- tests: `ppp_auth_probe_test`는 합성 frame만, `ppp_auth_trial_test`는 OS/child/observer 경계를 번역 단위 안에서 fake로 치환하며, `ppp_trial_observer_test`는 CF dictionary와 fake SC/process/interface 조회만 사용합니다. trial 테스트의 macro 정의 앞 wrapper는 별도 링크된 실제 공용 preflight를 호출하되 모든 callback과 readlink는 fake입니다. 정상 fixture는 설정·`.ppprc` leaf에 ENOENT를 반환하고 매핑된 `.ppprc`의 present/symlink/access error와 모든 시스템 항목의 차단을 회귀 검사하도록 추가했습니다. `NULL` backend도 실제 host 조회로 빠지지 않습니다. SDK compile-only·일반·ASan/UBSan·CLI help/invalid가 `verify-offline`에 연결돼 있습니다. 이번 home/alias/진단 수정의 fake 회귀 검사는 위 r2 일반·ASan/UBSan gate에서 통과했습니다. 실제 재시험은 하지 않았습니다.

근거는 [RFC 1662](https://www.rfc-editor.org/rfc/rfc1662), [RFC 1661 §5.1](https://www.rfc-editor.org/rfc/rfc1661), Apple 공개 pin `06a73c498c93551b7b40bf08424d437c0d3985e5`의 [`sys-MacOSX.c`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/sys-MacOSX.c) `InterfaceName` 게시입니다. 같은 파일의 `sys_init()`은 `nodetach`일 때(802행) 803행에서 `pid`를 게시하지만 `publish_dict`는 815행에서 만들고, `update_publish_dict()`는 NULL dict에 저장하지 않으므로 pin 기준으로는 게시 항목에 `pid`가 없다고 추론합니다. 그래서 `pid`는 필수 조건이 아니라 있을 때만 대조합니다(아래 「SC 귀속 원인과 새 규칙」). 설치본 1029와 공개 1027의 동일성은 미확인입니다.

## 상태와 근거

| 구분 | 내용 |
|---|---|
| 실장치에서 확인 | 고정 `AT+CGDATA=?` 지원 조회를 한 번 실행해 `PPP` 보고를 관측했습니다. 별도 `AT+CGDCONT?` 설정 조회에서 CID 1의 `IPV4V6` 및 `<carrier APN>`을 관측했습니다. 두 결과는 서로 다른 조회이며 실제 접속은 확인하지 않았습니다. 아래 조회 계약 변경의 실기 검증은 아직 없습니다. |
| 조회 계약 | `--probe-status`, `--probe-pdp`, `--probe-data-support`는 송신 전에 IN을 읽지 않습니다. 자기 에코보다 앞선 완결된 줄은 내용 분석 없이 건너뛰며(`pre_echo_skipped`), 그중 자기 prefix만 mismatch입니다. 이 건너뜀은 장치 큐를 비우는 동작이 아닙니다. 읽기는 자기 에코 뒤의 최종 결과 줄에서만 끝나고, 그 앞의 최종 결과 줄로는 끝내지 않습니다. timeout·오류·미확인·`ERROR`/`+CME ERROR`면 같은 실행의 다음 명령을 보내지 않습니다. timeout은 조용한 결과가 아니며, SDK 주석대로 timeout 뒤 data toggle 재동기화가 필요할 수 있어 같은 실행에서는 close·release만 합니다. 다음 실행의 안전은 보장하지 않습니다. 4회·2초는 명목상 상한이며 동기 SDK 호출의 wall-clock 상한이 아닙니다. `matched`여도 요청 귀속의 완전한 증명이 아니며, 에코를 끈 모뎀은 미확인입니다. SIM·망·APN·`NO CARRIER`의 원인은 이 조회로 확정하지 않습니다. |
| 확인하지 않음 | 실행형 `AT+CGDATA=...` 접속, PPP 프레임 교환, IP 할당, 인터넷 접속은 확인하지 않았습니다. 설정·지원 응답만으로 접속 성공을 뜻하지 않습니다. |
| 현재 구현 | 생산 경로는 USB 진단과 고정 조회 명령까지입니다. USB bulk와 `pppd`를 잇는 브리지나 네트워크 설정 코드는 없습니다. |
| 오프라인 구현 | 생산 경로와 분리된 byte core, 실제 로컬 child/pipe, supervisor, 실제 SDK IF2 어댑터가 있습니다. 같은 어댑터와 guarded session의 fake SDK/registry 검사이며 실제 장치 연결·`pppd`·IOKit 동시성 검증은 아닙니다. |

표준이 정하는 연결 동작과 DJI 장치에서 확인한 동작은 구분합니다. 아래 내용은 구현 완료나 실행 승인이 아닙니다.

## `AT+CGDATA` 상태 전이

[ETSI TS 127 007 V19.6.0 §10.1.12](https://www.etsi.org/deliver/etsi_ts/127000_127099/127007/19.06.00_60/ts_127007v190600p.pdf)는 실행형 명령을 다음 형식으로 정의합니다.

```text
+CGDATA[=<L2P>[,<cid>[,<cid>[,...]]]]
```

실행형 명령이 성공하면 모뎀은 `CONNECT`를 내고 V.250 online data state로 들어갑니다. 이때 호스트는 AT 응답 줄을 계속 파싱하는 대신 데이터 경로를 다뤄야 합니다. `AT+CGDATA=?`는 지원 목록 조회일 뿐 이 전이를 수행하지 않습니다.

표준 §10.1.12 인쇄 288쪽은 데이터 전송이 끝나고 계층 2 프로토콜 종료 절차가 성공적으로 끝나면 V.250 command state로 돌아와 최종 결과 코드 `OK`를 반환한다고 명시합니다. 오류나 접속 시작 실패에서는 command state로 돌아와 `NO CARRIER` 또는 활성화된 CME 오류가 나올 수 있습니다. 따라서 성공한 L2P 종료 뒤 AT 복귀는 표준 계약에 포함됩니다.

[RFC 1661 §§3.7, 5.5](https://www.rfc-editor.org/rfc/rfc1661.html)는 PPP의 LCP 종료 교환을 정의합니다. 이는 PPP 링크 계층 종료이며, 셀룰러 PDP context 비활성화나 모뎀의 AT 복귀 절차 전체를 대신 정의하지 않습니다. 표준 계약과 이 DJI 모듈의 실제 종료 동작은 별개이며 후자는 아직 검증하지 않았습니다.

| 종료 사건 | 설계상 해석 |
|---|---|
| LCP 종료 완료 및 표준 결과 확인 | AT command state 복귀를 확인하는 목표 조건 |
| `NO CARRIER` 또는 CME 결과 | 접속 실패/종료 신호로 기록하고 세션 결과를 판정 |
| USB read/write 오류, 프로세스 종료, EOF | 전송 경로의 오류일 뿐 AT 복귀 증거가 아님 |
| timeout 또는 강제 close | 상태 불명. 재송신이나 임의 복구 명령으로 성공 처리하지 않음 |

제조사 전용 문서는 현재 확인 자료가 아닙니다. 이를 오프라인 core 구현의 선행 필수조건으로 두지 않습니다. 실제 연결 동작은 향후 별도 승인과 검증이 필요합니다.

## PPP 바이트 스트림

[RFC 1662 §§4.1–4.2](https://www.rfc-editor.org/rfc/rfc1662.html)는 PPP HDLC-like framing의 ordered octet stream을 정의합니다. Flag는 `0x7e`, Control Escape는 `0x7d`이며 escaping은 해당 옥텟과 `0x20`의 XOR를 사용합니다.

브리지는 이 바이트를 프레임으로 해석하거나 변형하지 않습니다. PPP framing, FCS, 인증 및 LCP/IPCP/IPv6CP는 `pppd`가 담당합니다. USB read 크기나 endpoint의 512바이트 `maxPacket`은 PPP 프레임 경계나 MTU가 아닙니다. 한 PPP 프레임이 여러 read로 나뉘거나 여러 프레임이 한 read에 들어올 수 있습니다.

바이너리 스트림에 문자열 파싱, CR/LF 분할, 텍스트 로그 출력은 적용하지 않습니다. `CONNECT`는 제어 상태에서만 인식합니다. 전환 경계에 같은 USB read의 뒤쪽 바이너리 데이터가 있으면 바이트 offset을 보존해 데이터 경로로 전달해야 합니다.

## 제어 응답과 데이터 경계

기존 조회 parser는 정해진 응답 줄을 필터링해 출력하는 용도입니다. 완전한 `CONNECT` 줄의 끝 위치와 같은 버퍼의 tail offset을 반환하지 않으므로 연결용 parser로 재사용할 수 없습니다.

오프라인 core 후보는 확인된 CRLF 경계만 모델링합니다. 이는 관측/표준 근거로 정한 범위이지, 실제 모듈 구현이 이 규칙만 지원한다고 확정한 것은 아닙니다.

- 완성된 `CONNECT` 줄(속도 숫자 접미 허용)을 찾으면 제어 상태에서 데이터 상태로 전이합니다.
- `CONNECT`가 여러 read에 걸쳐 나뉘어도 모든 가능한 분할 지점에서 같은 전이를 내야 합니다.
- 전이 줄 뒤에 같은 read에 붙은 tail은 소비 offset으로 표시하고 한 번만 forwarding 합니다.
- tail은 다음 read를 기다리지 않고 자체 큐에 보존합니다. 다음 계층이 준비되기 전 먼저 전달해 버리지 않습니다.
- binary data 구간에서는 `CONNECT` 문자열이나 CR/LF를 탐색하지 않습니다.
- 불완전한 구분자나 partial delimiter의 의미를 임의 추정하지 않습니다. 허용하지 않은 형식은 제어 버퍼 정책에 따라 실패 또는 불명 상태로 둡니다.

후보 상태는 다음과 같습니다.

| 상태 | 의미 |
|---|---|
| `CONTROL_WAIT` | AT 제어 응답을 받고 정확한 전이 줄을 기다림 |
| `DATA_STREAM` | 바이너리 바이트를 양방향 전달 |
| `STOPPING` | 정상 종료에서는 새 세션과 상위 데이터 전달 시작을 멈추고, PPP 종료 교환은 bounded drain으로 유지합니다. hard I/O failure 또는 종료 제한 초과는 `RECOVERY_UNKNOWN`으로 처리하며, 이를 정상 종료로 간주하지 않습니다. |
| `CLOSED` | worker 종료와 자원 close가 끝남 |
| `RECOVERY_UNKNOWN` | 전송/종료 결과가 불확실해 정상 AT 상태를 주장할 수 없음 |

## 브리지와 `pppd` 후보

현재 후보는 고정된 USB bulk 브리지 실행 파일과 `pppd notty`를 잇는 구조입니다. 권장 후보일 뿐 구현되지 않았고 실제 동작도 확인하지 않았습니다.

로컬 `pppd(8)` 매뉴얼(`/usr/share/man/man8/pppd.8`)에 따르면 `notty`는 내부 PTY와 character shunt를 사용해 stdio를 연결할 수 있습니다. `pty` 방식은 `/bin/sh -c`를 통한 명령 실행 계약이 있어, 셸 문자열·인용·인자 전달을 따로 안전하게 설계해야 합니다. 직접 PTY를 만들면 termios, 권한, flush, 종료 책임이 추가됩니다.

검토한 Apple 소스의 순수 `notty` 경로에서는 시작 시 PTY `TCSAFLUSH`·PPP line discipline `ttyflush` 뒤 첫 shunt stdout이 나옵니다. shunt stdin과 nonblocking 설정도 그 출력 전에 끝납니다. stdout이 로그·설정·plugin·script에 오염되지 않는 고정 경로라면 첫 출력 이후 접속을 시작하고 CONNECT 뒤 stdin을 공급하는 순서는 시작 입력 폐기를 피하는 조건부 근거가 됩니다. 첫 stdout을 pending-TX에 보존하는 gate를 구현했지만 설치본 동작 대응은 미검증입니다. 이후 오류·종료 flush, LCP·인증·IP 성공이나 향후 정상 동작 보장은 아닙니다. 고정 sleep과 exec handshake만으로 준비를 판정하지 않습니다.

이 호스트에는 `/usr/sbin/pppd`가 root 소유의 non-setuid 파일로 있습니다. 설치본의 실제 버전, 이 바이너리와 확인한 Apple 소스의 동일성, 실행 가능 여부, 커널 PPP 지원은 확인하지 않았습니다. 로컬 매뉴얼은 root 권한이 필요한 오류 코드 3과 kernel PPP 지원 오류 코드 4를 설명합니다. 필요한 권한이 있을 수 있지만 자동 `sudo`나 권한 상승은 승인된 것이 아닙니다.

### 후보 옵션과 설정 영향

`ppp_launch_prepare()`는 `/usr/sbin/pppd`, cwd `/`, 빈 env `{NULL}`과 고정 argv `notty nodetach nopersist noauth noipdefault nodefaultroute noipv6 nolog refuse-pap refuse-chap refuse-mschap refuse-mschap-v2 refuse-eap noacsp`를 담은 정책 객체를 제공합니다. `noacsp`는 2026-10-17 다섯 번째 실기 근거로 더했습니다. 공개 소스 옵션 정의를 확인한 준비 API이지 설치본 지원 검사나 실행 함수가 아닙니다. 기존 `ppp_child_start()`의 explicit argv/env/cwd 계약에 사용할 수 있지만 production CLI에는 연결하지 않습니다. 사용자 문자열·셸·plugin·file/call·debug/record/kdebug/dump와 `usepeerdns`를 추가하지 않습니다. fixture 실행 파일 치환은 테스트 코드에만 있습니다.

다섯 `refuse-*` 옵션은 공개 pin의 같은 [`auth.c`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/auth.c)의 옵션 정의와 `auth_reset()` 허용 목록 반영을 근거로 추가했습니다. 이 pin의 거부 정책과 설치본 1029의 옵션 지원·wire 동작은 별개입니다. `auth-policy=unverified`, `interface-cleanup=unverified`, `installed-source=unmatched`를 유지합니다. 기존 `noauth`만의 정책은 상대 인증 정책을 제어할 뿐, peer가 로컬 인증을 요구할 때 협상 실패나 인증 정보 미전송을 보장하지 않았습니다. 아래 hostname·기본 EAP 경로는 그 기존 정책의 한계이며 새 `refuse-*` 정책이 이를 계속 허용한다는 뜻이 아닙니다. 공개 pin `06a73c498c93551b7b40bf08424d437c0d3985e5`의 [`auth.c`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/auth.c#L1489-L1496)는 비밀번호/secret이 없을 때 PAP/CHAP을 인증 허용 목록에서 제외합니다. 같은 파일 [`auth.c`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/auth.c#L1390-L1394)는 사용자명이 없으면 hostname을 기본값으로 둡니다. EAP 허용 기본값은 [`auth.c:1494-1496`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/auth.c#L1494-L1496)에 있습니다. [`lcp.c`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/lcp.c#L1688-L1765)는 협상 중 PAP/CHAP 요청을 EAP로 NAK하거나 EAP를 수락하는 경로를 보여줍니다. 이는 코드상 허용·협상 경로의 근거이지 EAP 패킷이 실제 전송된다는 확인은 아닙니다. 그러므로 고정 argv만으로 wire 상 인증 정책이나 신원 전송을 확정할 수 없습니다. 설치본 `1029.0.0.0.1`에 대응하는 소스를 찾지 못했으므로 이 판정도 검토한 pin의 동작이지 설치본의 확인된 동작은 아닙니다. `record`, `debug`, `kdebug`는 스트림 또는 민감 정보를 남길 수 있어 사용하지 않는 후보입니다. `nolog`도 모든 syslog 출력을 끄는 뜻으로 쓰지 않습니다.

`ppp_launch_preflight()`는 NULL backend일 때 함수 안에서 `lstat/getuid/getpwuid`를 선택합니다. executable, `/etc/ppp/options`·`postoptions`, 실제 UID home `.ppprc`, `pap/chap/srp-secrets`, `ip-up/down`·`auth-up/down`·`ipv6-up/down`의 metadata만 확인합니다. `HOME`을 신뢰하지 않고 파일 내용이나 개인 경로를 결과에 저장/출력하지 않습니다. 알려진 `/etc`→`/private/etc` 매핑은 canonical 고정 경로로 검사하되 다른 parent/final symlink는 차단합니다. 부재·존재·접근 오류·symlink·잘못된 타입·사용자 조회 실패를 항목별 고정 label로 보고합니다. ENOTDIR/권한 오류는 부재가 아닙니다. executable이 실행 비트가 있는 regular file이고 나머지가 모두 부재일 때만 metadata clear이며 설정/hook/secrets 존재나 불명확 상태는 차단합니다.

순수 `notty`에 적용되지 않는 tty-suffix options, argv로 요청하지 않는 peers/call 경로와 connect-errors/resolv.conf 산출물은 이 고정 입력 목록에 포함하지 않습니다. system options→실제 UID user options→argv→controller(해당 시)→Apple postoptions→channel extra options 순서이므로 빈 env/고정 argv만으로 앞선 config/plugin 통제를 보장하지 못합니다. clear 결과는 metadata→exec 경쟁 조건, 설치본 소스 대응/옵션 지원, 민감 시스템 로그 보호 또는 실제 실행 허가를 해결하지 않습니다.

사용자는 공개 pin 기준으로 남는 실행 사용자·uid와 접속 뒤 IP·DNS 시스템 로그를 허용했습니다. 진단의 `sensitive-log=unsuppressed`는 그 허용과 미억제를 뜻하며 실행 허가가 아닙니다. `execution=unapproved`는 metadata clear 뒤에도 유지합니다. `auth-policy=unverified`와 `interface-cleanup=unverified`도 실행 허가가 아닌 진단 label입니다. 실행이 가능해지면 시작 직전에 `ppp_launch_preflight(NULL)`의 고정 label만 확인합니다. present·access-error·symlink·wrong-type·user-error는 차단입니다. 이 수정에서는 호스트를 조회하지 않았습니다. 설치본 pppd는 non-setuid라 root가 필요합니다.

사용자는 고정 정책 1회와 종료 뒤 PPP 인터페이스를 내리는 범위를 승인했지만, 실행 코드에는 생성된 인터페이스의 소유를 식별하거나 종료 뒤 해당 인터페이스가 내려갔음을 검증하는 기능이 없습니다. 따라서 약속한 정리 조건을 충족하지 못해 실제 실행은 보류합니다. 기본 경로·DNS 설치·NAT·공유·다른 인터페이스 변경은 범위 밖이며 인터페이스·주소·peer route는 생길 수 있습니다.

기존 `noauth`만의 정책에서는 공개 pin의 hostname 기본 사용자명과 기본 EAP 허용·협상 경로 때문에 사용자명·암호를 명시하지 않아도 신원 미전송이나 인증 실패를 보장하지 못했습니다. 새 다섯 `refuse-*` 옵션은 이 인증을 거부하도록 의도하지만 설치본 소스·옵션 지원·동작은 확인하지 않았습니다. 설치본의 인증 식별값 미송신을 보장하지 않습니다. 공개 pin의 `main.c`에는 정상 종료 시 `die`에서 cleanup을 거쳐 `sys_cleanup`과 `disestablish_ppp`로 가는 경로가 있습니다. 그러나 kernel이 마지막 socket close 때 인터페이스를 내리는 동작과 설치본 1029의 대응은 확인하지 않았으므로 모든 종료 상황에서 인터페이스가 내려간다고 단정하지 않습니다. charshunt의 세션 탈출은 미확인이며 직접 자식 종료는 후손 종료 증거가 아닙니다. 자체 검사는 fake metadata backend만 사용하며 호스트 내용이나 실제 pppd를 읽거나 실행하지 않습니다.

공개 핀 `06a73c498c93551b7b40bf08424d437c0d3985e5` 소스 판정: `utils.c`의 `log_write`는 `sys_log`를 먼저 호출하며, `log_to_fd`가 음수일 때만 fd 쓰기를 건너뜁니다. `sys-MacOSX.c`의 `sys_log`는 `vsyslog`를 사용하고, NetworkExtension controller가 활성이면 `os_log_with_type`을 사용하며 `nolog`를 참조하지 않습니다. `main.c`는 시작 시 `"started by %s, uid %d"` 형식으로 실행 사용자와 UID를 로그합니다. `ipcp.c`는 `notice()`로 로컬·원격 IP와 기본·보조 DNS를 억제 옵션 없이 기록합니다. 설치본 `LC_SOURCE_VERSION 1029.0.0.0.1`은 소스 tag `ppp-1027.121.1`과 다르며, 1029에 대응하는 소스는 확인하지 못했습니다. 이 연결은 실행 허가, 설치 바이너리 동일성 확인, 민감 로그 억제, TOCTOU 수정을 뜻하지 않습니다. 사용자의 고정 1회 승인은 받았지만 인증 정책과 인터페이스 정리 조건을 충족하지 못해 실제 실행은 보류합니다.

기본 경로와 DNS 설치를 제한해도 시스템 네트워크 변경이 없다고 볼 수 없습니다. 주소·인터페이스·peer route·SystemConfiguration 등은 바뀔 수 있습니다. 따라서 PPP 실행 전 네트워크 영향과 복원 범위에 대해 별도 승인이 필요합니다.

## 수동 로컬 메타데이터 점검

`make build/ppp_local_check`로 생산 앱과 분리된 명령을 빌드합니다. `./build/ppp_local_check --help` 또는 `./build/ppp_local_check --check-host`만 단독으로 받습니다. 도움말은 0, 내부/출력 오류는 1, 인자 오류는 2, 점검 후 실행 차단은 3입니다. 도움말·인자 오류는 host callback을 호출하지 않습니다. `--run` 분기나 수동 접속 함수는 없습니다.

점검은 UID/EUID/GID/EGID 중 하나라도 0이면 `identity=root`로 먼저 거절합니다. UID root만이 아니라 root group(GID/EGID 0)도 포함하는 보수적 차단입니다. UID≠EUID 또는 GID≠EGID는 `identity=elevated`로 거절합니다. `/`, `/usr`, `/usr/sbin`, 고정 `/usr/sbin/pppd`를 lstat로 확인하고 symlink·불명·잘못된 타입·root 이외 소유·group/other 쓰기를 차단합니다. 실행 파일은 regular·실행 비트·non-setuid/non-setgid도 요구합니다. 통과 뒤 `ppp_launch_preflight(..., NULL)`와 기존 diagnostic을 재사용하며 파일 내용·실제 UID·home·계정명·개인 경로를 출력하지 않습니다. ENOENT 외 오류는 부재로 취급하지 않습니다. 기존 `policy execution=unapproved`는 정책 진단이고, 마지막 `local execution=blocked`가 명령의 최종 판정입니다. 실패·생략을 clear로 합성하지 않습니다.

nonroot/non-setuid 실행 파일 조건이 확인되면 `privilege=unavailable`이며, 조건을 확인하지 못하면 `privilege=not-checked`입니다. 어느 경우에도 실행하지 않습니다. metadata clear도 `execution=blocked system-impact=unproven installed-source=unmatched`이고 인증 정책·인터페이스 정리는 미검증입니다. 자체 pipe 상대와 `notty`, `noipdefault`·`nodefaultroute`만으로 시스템 변경을 막았다는 근거가 없습니다. 공개 pin의 `dryrun` 이전에도 옵션 처리·권한·PPP 지원 검사 경로가 있습니다. 공개 pin과 설치본의 일치도 확인하지 못했으므로 설치 `pppd` 실행은 보류합니다. 실제 연결은 허용할 권한·PPP 인터페이스/주소 등의 영향·정리 확인 방법과 별도 승인이 필요합니다.

`tests/ppp_local_check_test.c`는 같은 checker를 include하고 OS metadata/identity와 기본 preflight 호출만 번역 단위 안에서 치환합니다. NULL 기본 선택과 명시적 fake backend를 나눠 검사하고 기존 preflight에는 fake metadata를 제공합니다. launch ABI·argv·diagnostic은 바꾸지 않습니다. 일반·ASan/UBSan·verify-offline은 fake-only 검사이며 실제 `--check-host`나 설치 `pppd`, USB·네트워크·sudo·시스템 로그를 실행하지 않습니다. 이 구현은 수동 접속 경로 완성이나 실제 LTE 시험 완료가 아닙니다.

2026-10-16 개발자의 검사 로그는 `build/ppp-local-check-20261016/`이며 `verify_exit=0`입니다. 일반 16개·ASan/UBSan 16개 모두 `failures=0`, checker는 각각 3,018 checks였습니다. SDK compile-only 타깃은 기존 최신 오브젝트를 재사용했고 이번 재컴파일은 없었습니다. CLI/API 가드를 마쳤으며 기존 잘못된 인자 65건과 checker 1건 모두 exit 2였습니다.
별도 수동 `--check-host`는 개발자가 한 번 실행해 rc 3, stderr 빈 파일을 확인했습니다. policy의 metadata는 clear, executable은 present이고 options/postoptions/user-options·PAP/CHAP/SRP·모든 hook은 absent였습니다. 최종 local은 `metadata=clear identity=nonroot executable=clear preflight=clear privilege=unavailable execution=blocked system-impact=unproven installed-source=unmatched auth-policy=unverified interface-cleanup=unverified`입니다. 이 결과는 점검 시점 metadata만 관측한 것이며 TOCTOU 해결·시스템 무영향·협상을 입증하지 않습니다. 실제 `pppd`·USB·LTE·sudo·시스템 변경은 실행하지 않았습니다.

## 전송 core의 책임과 오류 처리

전송 core는 생산 CLI, 장치 검색, USB IOKit, `pppd` 실행과 연결하지 않는 순수 오프라인 모듈로 제안합니다. 테스트에서 fake reader/writer와 fake sink/source를 주입합니다. 실제 USB backend나 장치 접근 권한을 추가하지 않습니다.

- USB 수신과 stdio 송신, stdio 수신과 USB 송신 사이에 각각 bounded queue를 둡니다.
- partial stdio write가 나면 남은 offset을 보존합니다. `EINTR`은 재시도하고 `EAGAIN`은 준비 상태가 올 때까지 같은 offset을 유지합니다.
- queue가 가득 차면 backpressure를 적용하거나 명시적 실패로 끝냅니다. 무제한 메모리 증가나 조용한 바이트 손실은 허용하지 않습니다.
- USB read 오류에서는 API가 반환한 buffer/size를 무시합니다.
- USB write 실패 시 전달 여부는 `unknown`입니다. 같은 바이트를 자동 재송신하지 않습니다.
- 진단에는 방향별 byte count와 오류 label만 기록합니다. 원문, hex, payload는 기록하지 않습니다.
- 바이너리 data fd와 진단 log fd를 분리합니다. 로그가 PPP 바이트 스트림에 섞이지 않게 합니다.
- PPP escape/FCS/authentication 처리는 브리지에서 하지 않습니다.

core는 opaque byte stream을 중계하며 PPP frame을 분류하거나 LCP 종료를 직접 구현하지 않습니다. supervisor가 lifecycle 예산을 관리합니다. 정상 종료에서는 새 세션 및 상위 데이터 전달 시작을 멈추되 PPP 종료 교환은 정해진 bounded drain 시간 동안 전달합니다. drain 완료 뒤 worker를 깨우고 join한 다음 각 핸들과 버퍼를 한 번만 닫고 해제합니다. hard I/O failure나 drain timeout에서는 `RECOVERY_UNKNOWN`으로 전환하고 성공으로 취급하지 않습니다. close 중복 호출을 막고, join 전 객체를 해제하지 않습니다.

## 동시성 및 USB API 불확실성

동일한 IOKit interface handle에 대해 동시 full-duplex 호출의 thread-safety가 명시적으로 보장되는지 확인하지 못했습니다. 이를 unsafe라고 확정하는 것도 아닙니다. 설계 단계에서는 호출 직렬화 여부를 검증할 수 있게 경계를 두고, 테스트 모델만으로 실제 IOKit thread-safety를 주장하지 않습니다.

SDK 헤더의 `ReadPipeTO`/`WritePipeTO` 계약(2688–2691, 2714–2717행)에서 no-data/completion timeout은 요청이 bus에 queue된 뒤 기준임을 확인했습니다. 이 계약은 전체 호출의 wall-clock 상한을 보장하지 않습니다. 반복 timeout으로 빈 poll을 해도 안전하다는 보장도 확인하지 못했으며, SDK 문서에는 timeout 뒤 data toggle이 어긋날 수 있다는 경고가 있습니다. 비동기 OUT 등 기존 금지 API 가드는 유지합니다. IF2 어댑터의 정해진 세 호출점만 비동기 IN·Abort·event source 구현 예외이며 실기 허가가 아닙니다. child reap과 callback 반환을 모두 확인한 뒤에만 core join과 interface close/release를 수행합니다.

fake 테스트는 결정적 byte ordering과 취소/오류 정책을 검사합니다. 실제 스레드 스케줄링, IOKit의 동시 호출 보장, 장치의 timeout 의미를 증명하지 않습니다.

## 종료와 복구 경계

향후 정상 종료는 PPP의 graceful LCP 종료 흐름을 유지하고, 표준 종료 결과와 AT command state 복귀를 별도로 확인하는 설계가 적절합니다. 시간 초과는 `RECOVERY_UNKNOWN`으로 남깁니다. 임의 escape sequence, `ATH`, `DTR`, `AT+CGACT` 또는 다른 hangup fallback은 설계상 자동 실행하지 않습니다.

`pppd` 종료, stdio EOF, PTY close, USB close는 모뎀 AT 복귀나 PDP 비활성화의 증거가 아닙니다. 상태를 확인하려면 PDP active 여부와 AT command state 복귀를 각각 검증해야 합니다. 현재는 둘 다 확인하지 않았습니다.

실제 송신, PPP 실행, 장치 접근, route/DNS/인터페이스 변경은 각각 별도 승인 대상입니다. 이 문서는 어떤 새 AT 명령이나 네트워크 변경을 승인하지 않습니다.

## 구현한 오프라인 API 계약

`src/offline_stream.h/c`는 앱 링크 목록에 들어가지 않습니다. 기존 CLI·discovery·USB·고정 조회 경로를 바꾸지 않습니다. 모든 함수는 호출자가 직렬로 호출하고 callback은 재진입하지 않습니다. 구조체 필드는 stack 할당과 테스트 경계 검사에 공개한 것이며, 초기화 뒤 호출자가 바꾸지 않습니다.

- `offline_stream_init()`은 callback 5개와 서로 겹치지 않는 호출자 소유 RX/TX storage를 검증합니다. 각 큐는 1~65,536바이트이며 동적 할당하지 않습니다. storage와 context는 release까지 유효해야 합니다. 살아 있는 stream을 다시 초기화하지 않습니다. close/release callback 각각이 소유 자원 하나의 정리를 모델링합니다.
- `offline_queue_enqueue()`는 전부 수락하거나 공간 부족을 반환합니다. `peek()`는 연속 구간 하나를 반환하고 `consume()`는 그 구간의 성공 길이만 진행합니다. 용량·위치·누적 수락/전달 카운터를 검사합니다.
- `offline_stream_input()`은 실제 read가 아니라 read 완료 이벤트입니다. RX는 USB→stdio, TX는 stdio→USB입니다. 성공 read는 유효한 buffer 및 `1 <= count <= requested`를 요구합니다. 길이 0 성공과 초과 count는 오류입니다. USB read 실패는 buffer/count/requested를 보지 않고 상태 불명으로 고정합니다. stdio read의 `INTR`/`AGAIN`은 바이트를 소비하지 않습니다. EOF는 정상 복원의 증거가 아니므로 상태 불명입니다.
- 제어 구간은 printable ASCII와 CRLF만 받습니다. `CONNECT\r\n` 또는 V.250 속도가 붙은 `CONNECT <숫자 1~10자리>\r\n`이 완성되면 데이터 상태가 됩니다. 예외로, 고정 hook의 echo인 `AT+CGDATA="PPP",1` 뒤 bare CR 한 번만 허용합니다(ATE1 모뎀은 `<명령>\r` 다음에 `\r\nCONNECT\r\n`을 보냅니다). 이 한 번은 bare CR로 끝나는 echo 기준입니다. CRLF로 끝나는 printable 줄은 같은 문구라도 기존처럼 무시하며 이 한 번에 세지 않습니다. 다른 bare CR echo, 두 번째 bare CR echo, 소문자·다른 cid는 CONTROL 오류입니다. 결과 줄로 실패하면 종류(`error`/`no-carrier`/…/`cme`/`cms`)와 1~5자리 숫자 코드만 진단에 남기고 문구는 보관하지 않습니다. 빈 줄·다른 printable 줄은 전이 근거가 아닙니다. `ERROR`, `NO CARRIER`, `BUSY`, `NO DIALTONE`, `NO ANSWER`, `+CME ERROR:`/`+CMS ERROR:` 완성 줄은 실패입니다. 총 제어 2048바이트 안에서 전이를 못 끝내거나 구분자가 잘못되면 실패합니다. 이 형식 범위는 실제 모듈의 모든 응답 형식을 지원한다는 뜻이 아닙니다.
- 반환 `consumed`는 입력 시작에서의 정확한 offset입니다. 전이 뒤 tail은 RX 여유만큼 즉시 저장하며 `ready()` 전에 전달하지 않습니다. 공간이 부족하면 `OFFLINE_FULL`과 수락한 offset을 반환합니다. 호출자는 남은 suffix를 보유하고 큐 공간을 만든 뒤 **다음 입력보다 먼저** 그 suffix를 넣습니다. 이미 소비한 prefix를 다시 넣지 않습니다. core는 미수락 바이트를 소유하지 않습니다. 잘못된 제어 바이트 자체는 consumed에 포함됩니다.
- `ready()`는 데이터 상태에서 호출하며 양방향 write gate를 엽니다. 준비 전에는 RX를 저장하지만 TX 입력을 수락하지 않습니다. gate는 실제 `pppd` 준비를 확인하지 않는 명시적 모델 이벤트입니다. 정상 drain 전에 gate를 열어야 보관 바이트를 전달할 수 있습니다.
- `step()`은 동기 write callback을 최대 한 번 호출합니다. stdio partial 성공만 count만큼 소비하고 `INTR`/`AGAIN`은 offset을 유지합니다. 성공 0·초과 count·hard error는 실패입니다. USB 성공은 요청 전체 count를 요구합니다. USB write의 **모든 실패**, `INTR`/`AGAIN` 포함, 전달 여부 불명이며 같은 바이트를 재송신하지 않습니다. USB 성공 partial count도 계약 오류로 중단합니다. callback은 fake일 뿐 실제 USB API를 구현하지 않습니다.
- `stop(drain_ms)`는 데이터 상태에서 정상 취소와 bounded drain을 시작합니다. `CONNECT` 대기 중 취소는 제어 결과 미완성이므로 상태 불명으로 정리합니다. 0 예산과 deadline 덧셈 overflow는 실패입니다. STOPPING 입력은 raw 종료 교환을 계속 전달합니다. 새 application traffic과 종료 교환 구분은 호출자 책임이며 core는 PPP를 해석하지 않습니다. `poll()`은 감소한 시계와 deadline 도달을 실패로 고정합니다. write 완료 뒤에도 시간을 검사하되 이미 성공한 count는 기록합니다. frozen clock에 대한 wall-clock 보장은 없으며 모든 I/O/시간 진행은 호출자 이벤트입니다.
- `drain_complete()`는 deadline 이전이고 양방향 큐와 in-flight가 비었을 때만 종료 교환의 모델 완료를 받습니다. 이후 새 입력과 begin/write는 금지합니다. 완료 이벤트는 AT/PDP 복원을 증명하지 않습니다. 실제 thread 없이 `begin()`/`complete()` 이벤트로 in-flight를 세고, drain 완료 또는 실패 뒤 zero in-flight일 때 `join()`을 받습니다. 이후 `poll()`만 close/release를 각각 한 번 실행합니다. 실패한 close도 재시도하지 않고 release는 실행합니다.
- `RECOVERY_UNKNOWN` 뒤에는 입출력을 중단합니다. 남은 in-flight 완료와 join은 받아 정리할 수 있습니다. 첫 오류 label과 `recovery_unknown`은 `CLOSED` 뒤에도 유지합니다. 정상 cleanup도 `recovery=unproven`입니다. `CLOSED`는 AT command state나 PDP 비활성화를 뜻하지 않습니다.
- `diagnostic()`은 고정 상태/오류 label, 길이·카운터만 `snprintf()`로 제공합니다. payload·hex·제어 원문은 출력하지 않습니다. 반환은 NUL 제외 필요 길이이고 짧은 buffer도 capacity>0이면 NUL로 끝납니다. 이는 이 진단 함수의 출력 계약이지, 실제 장치·외부 프로그램·시스템 로그의 개인정보 비노출 증거가 아닙니다.

## 오프라인 core 테스트 범위

최소 테스트는 production backend 없이 fake 입력과 출력으로 수행합니다.

- 모든 바이트 `0x00`~`0xff`의 왕복 보존
- `CONNECT`의 모든 분할 위치와 정확한 complete-line 판정
- 같은 read에 제어 줄과 tail이 합쳐진 경우 offset 및 tail 보존, 단 한 번 forwarding
- PPP frame의 read 병합·분할이 있어도 바이트 순서와 수량 보존
- partial write, `EINTR`, `EAGAIN`의 offset 보존
- 취소, worker 수명, join 뒤 close 한 번만 실행
- queue overflow와 각 방향의 read/write 오류 정책
- binary payload, private marker, raw byte가 진단 출력에 나타나지 않음
- production CLI/discovery/USB/`pppd` backend가 테스트에서 호출되지 않음

이 core 테스트는 opaque byte stream과 모델 lifecycle 예산의 오프라인 계약만 검증합니다. core는 PPP frame을 분류하지 않습니다. supervisor가 core가 받아들인 바이트의 사본을 별도 진단 모듈(아래 「PPP 협상 단계 진단」)에 넘길 뿐이며, 그 결과는 core와 relay 판단에 쓰이지 않습니다. 테스트는 실제 장치 연결, 동시 IOKit 접근, PPP negotiation, 인증, IP 설정, PDP 상태 및 시스템 네트워크 영향을 검증하지 않습니다.

## 추적한 구현 근거와 남은 확인

`usb_observer`의 SDK `WritePipeTO` 호출은 한 곳입니다. probe wrapper는 고정 source-pointer·정확한 길이와 local copy를 그대로 요구합니다. 내부 owned session의 PPP wrapper는 별도 bounded span 정책이며, 어댑터가 바인딩한 supervisor의 ready+CONNECT gate 이후에만 사용합니다. probe에서 임의 buffer를 허용하지 않습니다.

Apple 공개 소스의 검토 기준은 [pppd `tty.c`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/tty.c), [`sys-MacOSX.c`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/sys-MacOSX.c), [`main.c`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/main.c), [`pathnames.h`](https://github.com/apple-oss-distributions/ppp/blob/06a73c498c93551b7b40bf08424d437c0d3985e5/Helpers/pppd/pathnames.h)입니다. 확인 범위는 pinned revision `06a73c498c93551b7b40bf08424d437c0d3985e5`입니다. 시스템에 설치된 `pppd` 바이너리가 이 소스와 일치하는지는 확인하지 않았습니다. 이 revision에서 `nolog`는 fd 로깅만 끄며 `sys_log` 호출은 남습니다. 설치본 `LC_SOURCE_VERSION=1029.0.0.0.1`과 검토한 공개 소스 tag `ppp-1027.121.1`은 다릅니다. 서명·문자열만으로 동작 동일성을 승인하지 않습니다. `main.c`의 실행 사용자와 `ipcp.c`의 IP/DNS 기록 경로를 포함해 민감 시스템 로그는 억제하지 않습니다. 사용자가 공개 pin의 실행 사용자·uid 및 IP/DNS 잔여 로그를 허용한 것과 설치본 로그 동작의 미검증은 별개입니다. 첫 stdout의 조건부 시작 근거와 설치본 실제 readiness 검증은 별개입니다. fixture `RDY`는 기존 주입 테스트에만 씁니다.

| 확인 항목 | 현재 근거 또는 공백 |
|---|---|
| 실행형 `+CGDATA` 접속·`CONNECT`·실제 PPP | 장치에서 실행하지 않음 |
| USB bulk와 `pppd`를 연결하는 전송 경로 | 오프라인 구현은 있으며 production에 연결하지 않음 |
| PPP 인증 정책 및 peer 응답 | 미검증 |
| 같은 IOKit handle full-duplex thread-safety | 명시 보장 여부 미확인 |
| `pppd` 실행/권한/kernel PPP | 실제 버전과 실행 모두 미검증 |
| route, DNS, interface, PDP 복구 동작 | 실행하지 않았고 미검증 |

이번 구현은 production과 분리된 core·로컬 child/pipe·supervisor 및 실제 SDK IF2 어댑터까지입니다. 장치 통신, 실제 `pppd` 실행, 네트워크 변경은 포함되지 않습니다. 다음 연결 단계는 별도 승인과 검증이 필요합니다.

## 실제 SDK IF2 어댑터 수명 계약

- `ppp_usb_acquire()`는 내부 API이며 production CLI에서 호출하지 않습니다. 세션이 service/device 참조와 열거한 interface service·interface182 참조를 보유하고 configuration을 복사합니다. 실패 경로는 획득한 자원만 정리합니다. observer의 stack context와 signal state를 빌리지 않습니다.
- 하나의 owner thread와 CFRunLoop에서 한 개의 async IN 및 4096바이트 owned buffer를 유지합니다. 완료 buffer를 소비하기 전 다음 IN을 제출하지 않습니다. callback은 결과만 보관하고 supervisor에 재진입하지 않습니다. pending은 callback 안에서 내리지 않고 SDK dispatch가 반환된 뒤 pump에서 내립니다.
- 성공 completion은 1..4096 및 caller capacity 범위만 전달합니다. 실패 count/data는 무시하고 count 0·초과·submission 실패·timeout·unplug는 오류입니다. IN no-data/completion timeout은 OUT timeout과 따로 받습니다(아래 「USB IN timeout과 오류 라벨」). `AGAIN`은 아직 완료가 없다는 뜻입니다. 0바이트 EOF나 OUT ZLP를 만들지 않습니다.
- OUT은 owner의 동기 SDK writer를 한 번 호출합니다. 요청 전체 성공은 장치가 실제 받은 양의 계측이 아닙니다. 실패 뒤 재송신·새 IN submission·복구 명령은 금지하지만 outstanding callback을 받는 pump는 유지합니다. queue 이후 기준 SDK timeout은 hard wall-clock deadline이 아니며 동기 OUT 동안 callback dispatch와 종료가 지연될 수 있습니다.
- quiesce는 신규 I/O를 막고 pending IN에만 Abort를 한 번 요청합니다. Abort 반환·실패·deadline은 quiet가 아닙니다. pending이 없거나 callback dispatch가 반환한 뒤만 quiet입니다. 취소 중 기대한 aborted completion(`kIOReturnAborted`, `kIOUSBTransactionReturned`)은 취소 완료로 다루고 다른 오류 및 먼저 발생한 오류는 유지합니다. 취소 중이 아닐 때의 aborted는 오류입니다.
- supervisor는 child reap **AND** peer quiet 뒤 기존 core join → poll/close/release를 호출합니다. 어댑터 close/release도 pending·dispatch를 재검사합니다. source를 loop에서 제거하고 interface close, source/loop release, session refs/buffer/context release 순서로 정리합니다. callback 미도착 때는 WAIT와 모든 소유권을 계속 보유합니다. caller가 대기를 포기해도 free할 수 없습니다.
- fake SDK vtable은 실제 interface182 시그니처를 사용하고 같은 `ppp_usb.c`·`usb_observer.c`를 실행합니다. delayed callback·Abort 성공/실패·callback 미도착 상태의 자원 보유·최초 오류·reap 장벽·CONNECT tail·backpressure·probe pointer allowlist를 검사합니다. callback 미도착 검사는 유지 상태를 확인한 뒤 fake callback을 명시적으로 완료해서 검사 자체를 정리합니다.

### supervisor 인계 전 호출자 정리

`ppp_usb_dispose_step(struct ppp_usb **adapter)`는 caller-owned 어댑터만 받습니다. 소유권은 호출자의 전제 조건이며 start 성공을 런타임에 감지하거나 강제하지 않습니다. 저장된 `a->supervisor`와 supervisor 필드는 읽지 않으므로 초기 검증 실패로 초기화되지 않은 supervisor에도 의존하지 않습니다.

| 획득·인계 상태 | 소유권과 정리 경로 |
| --- | --- |
| acquire 실패 | 핸들 NULL, 내부에서 획득한 자원만 정리 |
| acquire 성공, 미bind | 호출자 소유, dispose |
| bind 실패 또는 성공 뒤 start 전 | 호출자 소유 유지, dispose |
| start 동기 실패 | 호출자 소유 유지, dispose. 실패한 supervisor를 step하거나 그 필드를 초기화됐다고 읽지 않음 |
| start 성공 | supervisor 소유, 호출자 adapter/peer 별칭 폐기. 기존 supervisor step/cleanup만 사용 |
| 성공한 start 뒤 exec `ENOENT` | 비동기 child 오류. supervisor 소유이므로 caller dispose 금지, CLOSED까지 step |

호출자는 acquire와 같은 owner thread·CFRunLoop에서 직렬 호출하고 signal handler에서는 호출하지 않습니다. 잘못된 owner/RunLoop, pumping 중 호출, dispose 재진입은 `{released=0,error=1}`로 거절하고 객체·핸들·SDK 자원을 바꾸지 않습니다. 별도 `dispose_active` guard를 quiesce의 SDK 호출 전에 설정하므로 AbortPipe의 inline callback dispatch에도 재진입하지 않습니다. 기존 quiesce → poll_quiet → close → release를 재사용하며 pending Abort는 최대 한 번입니다. 새 IN/OUT·deadline·강제 해제는 없습니다.

| 반환값 | 호출자 행동 |
| --- | --- |
| `{0,0}` | WAIT. 핸들·source·RunLoop·session·context를 유지하고 callback 반환을 기다리며 같은 함수 재호출 |
| `{0,1}` | 오류를 보고하되 핸들 유지. 거절된 호출은 올바른 문맥에서 재호출하고 정상 문맥의 WAIT 오류도 quiet 전 해제 금지 |
| `{1,0}` | 로컬 정리 완료, 핸들 NULL. borrowed peer/context 폐기 |
| `{1,1}` | 로컬 정리 완료와 오류를 함께 보고. 핸들 NULL, borrowed peer/context 폐기. 물리 close나 장치·모뎀·네트워크 복원 성공으로 해석하지 않음 |

이중 포인터 `adapter == NULL`은 `{0,1}`, `*adapter == NULL`은 이미 완료된 no-op `{1,0}`입니다. `error`는 기존 sticky USB 오류와 quiesce/poll/close 오류를 보존하며 `released`와 독립입니다. close가 실패해도 기존 closed 상태와 참조 해제 계약을 따릅니다. 오류를 지역값에 저장한 뒤 free하고 이후 어댑터를 읽지 않습니다. 완료 호출의 반환 오류는 호출자가 보존해야 하며 후속 NULL no-op은 과거 오류를 기억하지 않습니다.

dispose 시작 후 재bind·재start는 금지합니다. API는 supervisor start 자체를 바꾸거나 이를 런타임에 차단하지 않으며 호출자가 이 계약을 지켜야 합니다. 반복 안전성은 NULL로 갱신되는 같은 소유 핸들에만 해당합니다. 복사한 stale raw pointer는 역참조·재호출하지 않습니다. start 성공 인스턴스에 이 API를 호출하면 계약 위반입니다. generic peer, supervisor, 실행 wrapper와 production CLI는 바꾸지 않습니다.

fake SDK 검사는 미bind·bind 실패·입력 검증/초기 clock/overflow/fork 전 인자 실패 뒤 정리, 반복 WAIT와 Abort 오류, inline Abort completion, callback 진입·반환 뒤 outer SDK dispatch의 재진입, pumping·다른 thread/RunLoop 거절, close 오류와 NULL 반복을 다룹니다. 참조는 quiet 전 유지하고 quiet 뒤 close/source 제거/release가 각 한 번임을 검사합니다. 초기 clock 실패/overflow는 복사한 test peer의 `now_ms`만 바꿉니다. pending+start 실패는 **test-only peer.read를 start 전에 직접 호출한 합성 상태**이며 production 시작 순서라고 주장하지 않습니다. 존재하지 않는 절대 path의 exec 실패는 start 성공 뒤 supervisor 소유인 별도 회귀입니다. 기존 completion 오류 분류·SIGPIPE 기대값은 완화하지 않습니다.

이 API는 실행 허가·자동 연결·PPP 인터페이스 귀속/복원을 추가하지 않습니다. `auth-policy=unverified`, `interface-cleanup=unverified`, `installed-source=unmatched`를 유지합니다.

고정 정책 준비와 내부 CGDATA one-shot hook은 구현했지만 실제 실행 함수·신규 CLI·항상 미지원 stub은 추가하지 않았습니다. `begin_connect`는 IF2에 고정 `AT+CGDATA="PPP",1\r`만 요청하며 사용자 명령 문자열을 받지 않습니다. owner thread/CFRunLoop·바인딩된 supervisor/context·CONNECT·ready·exec 완료·no-error·미접속·미중단 상태와 자체 attempt를 확인합니다. attempt는 SDK 호출 전에 고정하고 기존 `usb_session_write()`와 단일 SDK writer를 재사용합니다. probe source-pointer/정확한 길이/local-copy allowlist는 변경하지 않습니다. 동기 성공은 요청 완료일 뿐 실제 장치 수신량 계측이 아닙니다.

다음 실기 단계는 설치본 입력 준비/옵션·설정/plugin/hook·민감 시스템 로그 정책 확인 뒤 정확한 실행 명령·1회 송신·권한·PPP interface/address/peer route 영향·중단/복원 한계를 제시하고 별도 승인을 받는 것입니다. 이번 단계는 실제 CGDATA 송신·pppd 실행(`--version` 포함)·sudo·네트워크 변경을 하지 않습니다. LCP·AT 복원·PDP 상태는 EOF/reap/close/OK substring으로 판정하지 않고, 확인하지 못한 종료는 recovery unknown입니다.

### USB IN timeout과 오류 라벨

2026-10-17 세 번째 실기에서 CONNECT 뒤 PPP 데이터 단계(`phase-reached=session`, `modem-echo=seen`)에 들어간 지 얼마 안 되어 `error-ms=1287`, `supervisor-error=usb`, `stream-error=io`로 끝났습니다. payload는 `0/0`, `usb-input-bytes=260`, `child-output-bytes=160`, `control-bytes=39`였습니다. 원인은 확정하지 못했고 후보는 IN timeout, 0바이트 완료, OUT 실패입니다. 당시 IN timeout은 OUT과 같은 1000ms였으므로, 데이터 단계에서 1초 동안 IN이 조용하면 fatal이 됐을 수 있습니다.

- `ppp_usb_acquire(..., timeout_ms, read_timeout_ms, &adapter)`: `timeout_ms`는 OUT(`WritePipeTO`, CGDATA hook)용이며 NULL이면 1000ms입니다. `read_timeout_ms`는 `ReadPipeAsyncTO`의 no-data/completion 값이며 NULL이면 OUT 값을 씁니다(기존 단일 timeout과 같음). 둘 중 0은 거절합니다.
- `ppp_session_start`는 `connect_ms + session_ms + drain_ms + 1000`을 uint64로 계산해 IN timeout으로 넘깁니다. 0이거나 uint32_t를 넘으면 -1입니다. request 구조체는 바꾸지 않았습니다.
- IN timeout 완료는 계속 fatal입니다. 재제출·ClearPipeStall·reset·0 timeout은 쓰지 않습니다. SDK의 timeout 뒤 data toggle 경고 때문에 같은 IN을 다시 쓰지 않습니다. 실제 장치에서 다음 실행 전에는 동글을 다시 연결해야 합니다.
- 첫 오류만 종류와 위치를 고정 enum으로 남깁니다. 종류는 timeout·aborted·no-device·not-open·stall·overrun·zero-length·oversize·other, 위치는 submit·completion·write·connect·abort·validate입니다. OUT 실패는 `usb_session_write` 반환값으로 나눕니다(timeout → timeout, 장치 없음 → no-device, 그 밖 → other).
- 라벨은 바인딩된 supervisor에도 복사합니다. 어댑터가 먼저 해제되므로 session observe는 supervisor 쪽 enum만 읽습니다. 바인딩 전 오류는 라벨에 남지 않습니다. `ppp_lte_once`는 최종 관측이 유효할 때만 `usb-error`·`usb-error-site`를 출력하고, 아니면 `unproven`입니다. 이 요약 줄(stdout)에는 raw IOReturn·16진수·식별자를 넣지 않습니다. 기존 `usb_observer`는 OUT 쓰기 timeout·실패와 close 실패 때 stderr에 `IOReturn=0x…`를 계속 출력하며, 이 범위에서는 바꾸지 않았습니다. 송신 전 재확인 실패는 이유와 관계없이 `no-device/validate`이고 OUT의 점유·미개방은 `other`라서, 라벨은 대략적인 분류입니다.
- 이 변경은 오프라인 수정입니다. 2026-10-17 `make verify-offline`(r14)은 일반·ASan/UBSan 모두 exit 0, 실패 0입니다(ppp_usb 실패 0, lte-once 1,745, integration 1,575). fake SDK와 자체 fixture 검사이며 실제 장치 확인은 아직 없습니다.

## PPP 협상 단계 진단

4번째 실기(`6117274`)는 `usb-error=none`, `supervisor-error=timeout`, `phase-reached=drain`, `child-exit=code:5`로 끝났고 SC 게시는 없었습니다. 공개 pin `06a73c4`의 `sys-MacOSX.c` `commit_publish_dict()`는 `PHASE_RUNNING`(`auth.c` `np_up()`에서 시작)과 `protocols_ready`일 때만 게시하므로, SC 미게시는 IPCP가 Opened에 이르지 못했을 가능성을 가리킵니다. 멈춘 단계는 정하지 못했고 설치본 1029와의 동일성도 확인하지 않았습니다. 다음 실기에서 LCP·인증·IPCP 중 어디서 멈췄는지 보려고 수동 분류기를 더했습니다.

- 입력: `flush_pending`은 `offline_stream_input` 전후 `stream.tx.accepted` 차이만큼, 소비 구간의 끝부분을 `PPP_NEGO_LOCAL`로 넘깁니다. `read_usb`는 같은 방식으로 `stream.rx.accepted` 차이를 `PPP_NEGO_PEER`로 넘깁니다. CONTROL_WAIT의 제어 바이트는 큐에 들어가지 않아 차이가 0이고, 큐에 들어가는 바이트는 항상 소비 구간의 끝이라 이 계산이 정확합니다. 차이가 소비량보다 크면 넘기지 않습니다. 오류 정리 경로는 데이터가 없어 보지 않습니다.
- 초기화와 복사: `ppp_supervisor_start`가 구조체 초기화 직후 `ppp_negotiation_init`을 부르고, `ppp_session_observe`는 supervisor가 유효할 때만 요약 구조체(`struct ppp_nego_summary`)를 값으로 복사합니다. frame 앞부분 버퍼는 복사하지 않습니다.
- 분류: flag(0x7e)로 나누고 escape(0x7d, XOR 0x20)를 풀어 FCS-16 good residue(0xf0b8)를 확인합니다. 첫 flag 전 바이트는 버립니다. ACFC(`ff 03` 생략)와 PFC(1바이트 프로토콜)를 받아들입니다. 프로토콜은 LCP·PAP·CHAP·EAP·IPCP·IPV6CP·IPv4·IPv6·기타로, 코드는 프로토콜별 범위(LCP 1~11, NCP 1~7, PAP 1~3, CHAP·EAP 1~4) 안에서만 셉니다. 범위 밖과 데이터 프로토콜은 0번 칸입니다.
- 상태: LCP·IPCP·IPV6CP의 `opened`는 양방향 Configure-Ack가 누적으로 한 번 이상 보였다는 뜻입니다. NCP는 그다음 상대 Configure-Reject(`rejected`), 상대 Configure-Nak(`nak`), 어떤 frame이든(`configuring`) 순으로 정합니다. 인증 프로토콜은 상대 LCP Configure-Request의 option 3에서 두 바이트만 읽고, option이 잘렸거나 64바이트 앞부분을 넘으면 `unknown`입니다. 인증 결과는 상대가 보낸 성공·실패 코드만 반영합니다. LCP Terminate-Request는 보낸 방향을 bit로 남깁니다. `nak`·`rejected`도 `opened`처럼 누적 우선순위이며 현재 상태가 아닙니다. 상대 LCP Protocol-Reject는 code 개수에만 들어가고 NCP 라벨에는 반영되지 않습니다(`configuring`). `fcs_errors`는 FCS 불일치와 abort·runt를 함께 셉니다.
- 오류: 두 해석 모두 FCS가 틀리거나, abort(`7d 7e`)나 너무 짧은 frame이면 `fcs_errors`, 4096바이트를 넘으면 `too_long`으로 셉니다. 빈 frame(연속 flag)은 세지 않습니다. 모든 개수는 포화합니다.
- ACCM: 옵션 값을 읽지 않으므로 초기 ACCM대로 0x20 미만 비escape 바이트를 지운 해석(`strip`)과 그대로 둔 해석(`raw`)을 함께 계산합니다. `strip`이 맞으면 우선합니다. RFC 1662 §7.1에서 벗어난 진단용 선택이며, 우연한 FCS 일치(약 1/65536) 위험이 있습니다.
- 개인정보: frame마다 앞 64바이트만 두고 분류 뒤 0으로 지웁니다. 다음 flag 전 미완성 frame 앞부분은 실행 중 supervisor 메모리에 남습니다. `ppp_session_release()`는 해제 직전에 `ppp_negotiation_wipe()`로 이 앞부분을 volatile 쓰기로 지웁니다. 요약 카운터는 지우지 않습니다. 요약에는 개수와 enum만 있으며 payload, option 값, IP·DNS, 인증 이름·challenge·response, 원문·16진수는 없습니다.
- 출력: `ppp_lte_once`는 `overall=partial` 뒤에 `ppp-lcp`, `ppp-auth`, `ppp-auth-result`, `ppp-ipcp`, `ppp-ipv6cp`, `ppp-terminate`, `ppp-frames-rx`, `ppp-frames-tx`, `ppp-fcs-errors`, `ppp-long-frames`를 붙입니다. 최종 관측이 유효하지 않으면 모두 `unproven`입니다.
- 불변: 고정 CGDATA 바이트, 고정 launch argv(당시 14개, 이후 `noacsp`를 더해 15개)·env·cwd, USB 송신 정책, `peer_done` 미합성, once 정지·정리 순서, probe allowlist, auth probe 동작은 바꾸지 않았습니다. `ppp_auth_probe.c`는 FCS 계산 함수만 공유합니다.
- 검증: `ppp_negotiation_test`(합성 frame), `ppp_supervisor_test`(echo fixture로 LCP Ack·IPCP Nak 왕복, 관측 바이트 = accepted), `ppp_usb_test`(fake SDK에서 제어 문자열 제외 확인), `ppp_lte_once_test`(토큰 출력)를 더했습니다. 2026-10-17 `make verify-offline`(r16)은 일반·ASan/UBSan 모두 exit 0, 실패 0입니다(ppp_negotiation 76, lte-once 1,745, integration 1,575). 실제 장치 확인은 없습니다.

### SC 귀속 원인과 새 규칙

- 근거: 다섯·여섯 번째 실기 모두 `ppp-lcp=opened`, `ppp-auth=none`, `ppp-ipcp=opened`이면서 `reason=interface-unproven`이었습니다. 사용자 승인 아래 두 실행의 pppd 시스템 로그에서 고정 문구 `Committed PPP store`의 횟수만 셌고, 두 실행 모두 한 번 이상 있었습니다. 로그 원문은 열람하지 않았습니다. pin에서 이 문구는 `SCDynamicStoreSetMultiple()` 직전에 찍히고 호출부가 반환값을 보지 않으므로 게시 시도의 증거일 뿐입니다. 실패 지점은 귀속일 가능성이 큽니다. 위 4번째 실기의 "SC 게시 없음"도 같은 귀속 실패를 미게시로 읽었을 수 있으며, 그 실행의 로그는 확인하지 않았습니다.
- 원인(pin 기준 추론): 공개 pin `06a73c4` `sys-MacOSX.c`의 `sys_init()`은 `nodetach`일 때(802행) 803행 `publish_dictnumentry(kSCEntNetPPP, CFSTR("pid"), getpid())`를 815행 `publish_dict = CFDictionaryCreateMutable(...)`보다 먼저 부릅니다. `update_publish_dict()`는 `publish_dict`가 NULL이면 저장하지 않으므로 게시된 `State:/Network/Service/<uuid>/PPP`에 `pid`가 없습니다. 기존 `candidate()`는 `pid` 일치를 필수로 요구해 귀속이 성립할 수 없었습니다.
- `noacsp`: 원인이 아니었습니다. `noacsp` 없이 실행한 다섯 번째에도 `Committed PPP store`가 있었습니다. 고정 argv 15개에는 그대로 남깁니다.
- 새 규칙: 모두 만족할 때만 귀속합니다. (1) 직접 child가 살아 있고 `process_identity` 시작 식별자가 같음. (2) 시작 전 `before_services`가 있고, 거기 없던 PPP service key가 현재 정확히 하나(`pid` 없음·다른 `pid`·dictionary 아닌 값도 셈). 둘 이상이거나 이미 귀속한 key와 다른 새 key면 ambiguous. (3) 그 service의 `pid`는 없거나, 숫자이고 child PID와 같아야 함. 다르거나 숫자가 아니면 후보 아님(ambiguous 아님). (4) 시작 전부터 있던 key는 세지 않음. 단 현재 `pid`가 child PID와 같으면 ambiguous. (5) `InterfaceName`이 문자열·현재 유일·시작 전 없음·`if_nametoindex()` 존재. (6) 귀속한 뒤에도 같은 key에 다른 `pid`가 붙거나 값이 dictionary가 아니게 되면 ambiguous입니다. 매 sample의 `current_valid` 재계산, interface index가 바뀌면 ambiguous, reap/ownership loss 뒤 PID 재조회 금지, finish의 retained key 제거 확인, 진단 문구는 그대로입니다.
- 한계: 같은 때 다른 PPP 서비스가 새로 뜨면 모호로 멈추고 송신하지 않습니다. 잘못 귀속하지 않는 쪽을 택했습니다. `pid` 없는 서비스가 우리 것이라는 근거는 시작 전 snapshot과 유일성뿐입니다. 설치본 1029와 pin의 동일성은 미확인입니다. `ppp_trial_observer_test`에 fake 사례만 더했고 실제 장치에서는 확인하지 않았습니다.

### 일곱 번째 실기와 주소 소실 진단

- 결과(`c4d8086`): 처음으로 `attribution=unique-observed`였습니다. 그러나 `reason=address-missing`, `error-ms=4409`, payload `0/0`, TERM 1·KILL 0, `supervisor-error=core`, `ppp-lcp=opened`, `ppp-ipcp=opened`, `ppp-terminate=none`, frame rx13·tx13, `child-exit=code:5`로 끝났습니다.
- 코드 근거: `addresses()`는 `found == 0 && !pinned`면 대기(1)하고, `found != 1`이면 실패합니다. 형식이 틀린 IPv4나 둘 이상은 `found=2`로 `address-ambiguous`이므로, `address-missing`은 `found == 0 && pinned`일 때뿐입니다. 즉 귀속 인터페이스의 point-to-point IPv4 하나를 pin한 뒤 그 인터페이스의 IPv4가 사라졌습니다.
- pin 근거(원인 미확정): 공개 pin `ipcp.c` `ipcp_down()`은 demand가 아니면 2027행 `sifdown()`, 2028행 `ipcp_clear_addrs()`를 부르고, `ipcp_clear_addrs()`는 2062행 `cifaddr()`로 주소를 지웁니다. 주소 소실과 맞는 경로입니다. 무엇이 IPCP를 내렸는지(Terminate, 재협상 Configure-Request, LCP 재시작 등)는 출력으로 가릴 수 없었습니다. `supervisor-error=core`가 once의 network 실패 정지 뒤 정리 결과인지도 확정하지 않았습니다. 설치본 1029와 pin의 동일성은 미확인입니다.
- 진단 필드(`struct ppp_lte_network`, 판정에 쓰지 않음): `pinned_ms`는 처음 pin할 때의 `last_ms`(그 단계의 마지막 시계 읽기)이고, pin 전에는 0입니다. `failed_stage`는 첫 `fail()`에서 첫 `reason`과 같이 기록하는 그때의 `stage`이며 이후 `fail()`·abort가 덮어쓰지 않습니다. `init`이 `PPP_LTE_FAILED`(미기록)로 둡니다. DONE에서는 abort가 아무것도 하지 않으므로 성공 경로에서는 미기록으로 남습니다.
- 출력(`ppp_lte_once`, `ppp-long-frames` 뒤·observer 진단 앞, 순서 고정): `ppp-ipcp-req`, `ppp-ipcp-ack`, `ppp-ipcp-term`(IPCP code 1·2·5), `ppp-lcp-req`, `ppp-lcp-term`, `ppp-lcp-echo`, `ppp-lcp-protrej`(LCP code 1·5·9·8), `ppp-data`(IPv4 0번 칸)는 이미 모으던 `codes[방향][프로토콜][code]`를 `<상대>/<로컬>`로 냅니다. 최종 관측이 유효할 때만 숫자이고 아니면 `unproven`입니다. 이어서 `ip-ready-ms`(`pinned_ms - start_ms`, network 미시작·0·역행은 `unproven`, pin 전은 `none`)와 `network-stage`(실패면 `failed_stage`, 아니면 현재 `stage`, network 미시작은 `none`, 범위 밖은 `unknown`)를 붙입니다. Configure-Request 수에는 재전송이 들어가고, 개수에는 순서·시각이 없습니다.
- 불변: 고정 argv 15개, observer 귀속 규칙, USB/CGDATA, 인증 정책, network 판정 로직, once 정지·정리 순서는 바꾸지 않았습니다.
- 검증: `ppp_lte_network_test`(pin 뒤 주소 소실 `ADDRESS_LOST`에서 `address-missing`·`failed_stage=IP`·`pinned_ms`, 단계별 실패 stage, 반복 abort 뒤 sticky, 성공 경로 미기록), `ppp_lte_once_test`(fresh 값, 거절·final observe 실패의 `unproven`, network 미시작의 `none`/`unproven`, pin 뒤 `address-missing`의 숫자 `ip-ready-ms`와 `network-stage=ip`), `ppp_lte_once_integration_test`(실제 network 모듈의 `done`·`ip`·`none`) 사례를 더했습니다. fake·자체 fixture 검사이며 실제 장치에서는 확인하지 않았습니다.

### 여덟 번째 실기와 협상 순서 진단

- 결과: `attribution=unique-observed`, `reason=address-missing`, `ip-ready-ms=3242`, `network-stage=connect`, `error-ms=4361`, `ppp-ipcp-req=4/5`, `ppp-ipcp-ack=1/1`, `ppp-ipcp-term=0/0`, `ppp-lcp-req=2/1`, `ppp-lcp-term=0/0`, `ppp-lcp-echo=0/0`, `ppp-lcp-protrej=0/0`, `ppp-data=0/2`, frame rx13·tx13, TERM 1·KILL 0. pin 뒤 connect 단계에서 주소가 사라졌고, 어느 쪽도 Terminate-Request를 보내지 않았습니다.
- 가린 시스템 로그(시각·사건 종류만): 12.383 pppd IPCP up(local/remote, `Committed`), 13.474 SC 변경으로 IPMonitor 목록에서 ppp0 IPv4 제외, 13.485 pppd `Connection terminated`. 그 사이 pppd 로그 없음. 로그 시각과 once `-ms`는 기준 시계가 달라 맞대지 않았습니다.
- 후보(미확정): RFC 1661 상태표의 Opened + RCR → `tld` 뒤 재협상. pppd에서는 `ipcp_down()` → `sifdown()`·`ipcp_clear_addrs()` → `cifaddr()`입니다. Terminate 0·Req>Ack·중간 pppd 로그 없음과 맞지만, Req 수에는 Ack 전 재전송·Nak 뒤 재요청이 섞이므로 개수로는 가릴 수 없습니다.
- 순서 기록(`ppp_negotiation`, `struct ppp_nego_trace`): `nego_classify`가 FCS가 맞는 frame을 `codes[]`에 센 직후 `{count, direction, protocol enum, code}`를 남깁니다. 시각·payload·옵션 값은 없습니다. 64바이트 prefix와 wipe 정책은 그대로이고, wipe는 lane만 지우며 요약과 trace는 둡니다. 제어 frame(LCP·PAP·CHAP·EAP·IPCP·IPv6CP)은 하나가 사건 하나이고, 범위 밖 code는 기존 칸처럼 0입니다. 데이터(IPv4·IPv6·그 밖)는 마지막 제어 사건 뒤 구간(`gap`) 안에서 같은 방향·프로토콜 사건의 `count`만 올립니다. 그 구간 안 데이터끼리는 첫 등장 순서만 남습니다. 처음 48개만 저장하고, 하나라도 버린 뒤에는 모든 frame을 `dropped`로만 셉니다. 그래서 남은 사건은 실제 순서의 빈틈 없는 앞부분입니다. once 시험은 길어야 60초이고 물음은 처음 수십 frame 안에 있으므로, 구멍 뒤 꼬리보다 앞부분을 택했습니다. 개수는 포화(`UINT32_MAX`)합니다.
- 출력(`ppp_lte_once`): `network-stage` 뒤에 `ip-lost-ms`, 그 뒤 observer 진단 바로 앞에 `ppp-trace`를 붙입니다. 기존 토큰 순서는 그대로이고 가변 길이 토큰을 끝에 둡니다.
  - `ip-lost-ms`: `struct ppp_lte_network`의 `lost_ms - start_ms`. `lost_ms`는 첫 실패가 `address-missing`(pin 뒤 0개)이나 `address-changed`일 때 그 읽기의 `last_ms`이고, 그 밖에는 0(`none`)입니다. network 미시작·역행은 `unproven`. 판정에 쓰지 않으며, 실제 소실 시각이 아니라 처음 알아챈 읽기 시각입니다.
  - `ppp-trace`: `<p|l>.<lcp|pap|chap|eap|ipcp|ip6cp|ip|ip6|oth>[code][*count]`를 쉼표로 잇고, 버린 수가 있으면 `,+N`을 끝에 붙입니다. 최종 관측이 유효하지 않으면 `unproven`, 사건이 없으면 `none`. 버퍼는 48 × 최장 사건(`p.ip6cp255*4294967295,` 22자) + 여유로 잡아 잘리지 않고, 그래도 잘리면 `~`를 붙입니다. 출력에 `0x` 표기는 없습니다.
- 불변: 고정 argv 15개, observer 귀속 규칙, USB/CGDATA, 인증, network 판정 로직, once 정지·정리 순서, observer가 relay에 영향 없음.
- 검증 사례: `ppp_negotiation_test`(양방향 순서, 나쁜 FCS 미기록, 구간 안 데이터 접기, 새 제어 사건 뒤 새 구간, 범위 밖 code 0, 그 밖 프로토콜 접기, 개수 포화, 48개 초과 뒤 앞부분 불변과 `dropped` 포화, wipe 뒤 trace 유지), `ppp_lte_network_test`(`ADDRESS_LOST`·`ADDRESS_CHANGE`의 `lost_ms`, 그 밖 0), `ppp_lte_once_test`(9개 사건 문자열, 사건 없음 `none`, stale·미관측 `unproven`, 최악 길이 48개+`+N`이 `~` 없이 끝까지 출력, `ip-lost-ms` 숫자·`none`·`unproven`), `ppp_lte_once_integration_test`(실제 network 모듈로 connect 뒤 주소 소실 `ADDRESS_LOST`의 숫자 `ip-lost-ms`, 나머지 `none`, `ppp-trace=none`). fake·자체 fixture 검사이며 실제 장치에서는 확인하지 않았습니다.

### 아홉 번째 실기와 연결 전 주소 안정 대기

- 결과(`6460d96`): `reason=address-missing`, `ip-ready-ms=3323`, `ip-lost-ms=4416`, `network-stage=connect`. `ppp-trace=l.lcp1,p.lcp1,p.lcp2,l.lcp4,p.lcp1,l.lcp2,l.ipcp1,p.lcp11,l.ipcp1,p.ipcp1,p.ipcp4,l.ipcp3,l.ipcp1,p.ipcp1,p.ipcp3,l.ipcp2,l.ipcp1,p.ipcp2,l.ip*2,p.ipcp1,p.ipcp4,l.ipcp1,l.ipcp3,p.ipcp4,p.ipcp1`.
- 해석: `l.ipcp2`·`p.ipcp2`로 IPCP Opened, 바로 뒤 로컬 IPv4 frame 2개(connect 직후라 SYN으로 보지만 내용은 기록하지 않음), 그다음 상대 IPCP Configure-Request(`p.ipcp1`)입니다. RFC 1661 Opened + RCR → `tld` → pppd `ipcp_down()` → `cifaddr()`로 주소 삭제라는 여덟 번째의 재협상 후보와 맞습니다. Terminate(`ipcp5`·`lcp5`)는 없습니다. pppd 로그에서 모뎀 Request에는 IP-Address 옵션이 없어 pppd가 원격 주소를 기본값으로 대신 썼고(`defaulting to`), 그 1.1초 뒤 once의 TERM이 나갔습니다. 모뎀이 왜 재협상했는지, 재협상이 끝나 주소가 다시 붙는지는 확인하지 못했습니다. `ip-lost-ms - ip-ready-ms` = 1093ms는 처음 알아챈 읽기까지의 간격입니다.
- 동작 변경(사용자 승인): `addresses()`·`ppp_lte_network_step()`.
  - 읽기 결과 `found > 1`(둘 이상·형식 불일치·walk 상한)은 단계와 무관하게 `address-ambiguous` 실패.
  - `moved`(pinned인데 `found == 0`이거나 local/peer가 다름) && `stage == IP`: `pinned = 0`, `settle_start_ms = 0`, `stable_ms = 0`, `route_pending = 0`, `address_resets++`(포화) 뒤 1(대기). 바뀐 쌍은 이 읽기에서 pin하지 않고 다음 읽기에서 pin합니다.
  - `found == 0 && !pinned`: 1(대기), 기존과 같음.
  - `moved` && `stage >= OPEN`: 기존대로 `lost_ms` 기록 뒤 `address-missing`/`address-changed` 실패.
  - 새 pin: `settle_start_ms = last_ms`, `pinned_ms`는 0일 때만(처음 pin) 기록.
  - step에서 주소 확인과 그 뒤 `checked()`를 지난 다음, `stage == IP`이고 `last_ms - settle_start_ms < PPP_LTE_ADDRESS_SETTLE_MS`(3000)이면 0(대기). 통과하면 `stable_ms`가 0일 때 `last_ms`를 기록하고, 그제야 route socket 생성 → RTM_GET → 응답 검증 → `socket_prepare` → OPEN입니다. route 질의는 안정 판정 뒤 처음 시작합니다.
  - 안정 판정 뒤 RTM_GET 대기 중에 IP 단계 reset이 오면 `route_pending`을 버리고 route socket도 닫습니다. 새 대기 동안에는 그 소켓을 읽지 않습니다. 그래서 재협상 때 쏟아지는 커널 route broadcast가 수신 버퍼를 채우면, 나중의 RTM_GET 응답이 조용히 버려질 수 있습니다. 다시 안정되면 새 소켓을 만들고 새 sequence로 질의합니다. 응답의 sequence 검사는 이와 별개인 일반 방어이며 `ROUTE_SEQ` 테스트가 확인합니다.
  - `ip_deadline`(시작 + 30000)·`overall_deadline`(시작 + 60000)·`app_deadline`(socket 준비 + 10000)은 그대로입니다. 안정되지 않은 채 30초를 넘기면 `deadline()` → `checked()`의 `readiness-gate`로 끝납니다. connect는 1회, 재시도 없음.
- 필드: `settle_start_ms`는 판정용입니다. 진단 전용(판정에 쓰지 않음): `stable_ms`(현재 pin이 안정 판정을 통과한 `last_ms`, reset 때 0), `address_resets`(IP 단계 unpin 횟수, `UINT_MAX` 포화). `pinned_ms`는 처음 pin한 시각이고 다시 pin해도 덮어쓰지 않습니다. `lost_ms`는 실패로 끝난 소실(`OPEN` 이상)에만 기록합니다.
- 출력(`ppp_lte_once`, `ip-ready-ms` 바로 뒤): `ip-stable-ms`(`stable_ms - start_ms`, 0이면 `none`, network 미시작·역행은 `unproven`), `ip-resets`(`address_resets`, network 미시작은 `unproven`). `ip-ready-ms`의 `none` 판정은 현재 `pinned` 대신 `pinned_ms`를 봅니다. 대기 중 reset 뒤 실패해도 처음 pin 시각이 남습니다.
- 불변: 고정 argv 15개, observer 귀속 규칙, USB/CGDATA, 인증, once 정지·정리 순서, 전체 60초·앱 10초 예산, route 검증 엄격성, `IP_BOUND_IF`.
- 검증 사례(fake·자체 fixture, 실제 장치 미확인):
  - `ppp_lte_network_test`: 대기 중에는 step마다 fixture 시계를 100ms 더 진행하고, 안정 전 IP 단계에서 route socket·RTM_GET·TCP socket·connect가 0인지 매 step 확인합니다(`ADDRESS_BLIP_ROUTE`만 route socket·첫 질의 허용). `NONE`(pin 뒤 3초 뒤 진행, route 응답 수 = 질의 수), `ADDRESS_BLIP`(읽기 3-5 소실 → reset 1, 읽기 6에서 다시 pin, 그로부터 3초 뒤 성공), `ADDRESS_CHANGE`(읽기 6부터 local 변경 → reset 1, 새 쌍으로 3초 뒤 성공, 남은 pin이 새 local), `ADDRESS_BLIP_ROUTE`(첫 RTM_GET 대기 중 소실 → reset 1, route socket을 닫고 새 소켓·새 질의, 이전 응답 미도착, route socket 2개, 첫 질의만 미독), `ADDRESS_LOST`(pin 뒤 영구 소실)·`ADDRESS_FLAP`(약 1초마다 local 교대)은 `readiness-gate`·IP·socket/connect 0, `ADDRESS_DUPLICATE`·`ADDRESS_DUPLICATE_LATE`는 IP 단계에서 바로 `address-ambiguous`·reset 0, `ADDRESS_LOST_OPEN`은 OPEN에서 `address-missing`·connect 0, `ADDRESS_CHANGE_CONNECT`는 CONNECTED에서 `address-changed`·connect 1·send 0, `lost_ms`는 이 두 경우만 숫자.
  - 기존 기대값 변경: `ADDRESS_LOST`(pin 뒤 IP 단계 영구 소실)는 이제 바로 `address-missing`이 아니라 reset 1 뒤 30초 `readiness-gate`이며 `lost_ms=0`입니다. `ADDRESS_CHANGE`(읽기 6부터 변경)는 3초 대기 때문에 그 읽기가 IP 단계에 들어가, `address-changed` 실패에서 새 쌍으로 성공으로 바뀌었습니다. 연결 뒤 소실·변경 실패는 `ADDRESS_LOST_OPEN`·`ADDRESS_CHANGE_CONNECT`로 옮겨 검사합니다.
  - `ppp_lte_once_test`: fake network이 실제 모듈을 쓰지 않으므로 출력 토큰만 봅니다. 숫자 `ip-stable-ms`·`ip-resets=2`, pin 뒤 소실의 `none`·`0`, 미시작의 `unproven`.
  - `ppp_lte_once_integration_test`: 실제 network 모듈로 모든 pin 사례의 `ip-stable-ms ≥ ip-ready-ms + 3000`, `ADDRESS_BLIP`(대기 중 읽기 3-5 소실)의 echo 성공과 `ip-resets=1`, 그 밖 `ip-resets=0`, pin 안 된 `ADDRESS_TIMEOUT`의 `none`.

## 단발 실행 세션 계약

`ppp_session.h/c`는 하위 generic ABI를 바꾸지 않는 opaque heap 소유권 계층입니다. discovery·production CLI에는 연결하지 않으며 사용자 path/argv/env/cwd override도 없습니다. start는 입력 검증, metadata-only preflight, acquire와 bind를 동기로 수행합니다. 생성 중 핸들은 공개하지 않고 외부 동작 전부터 guard를 설정합니다. 이 동기 구간에는 취소점이나 SDK hard wall-clock 상한이 없습니다.

| API/상태 | 계약과 다음 동작 |
| --- | --- |
| `start(request, metadata_backend, **session)` | `*session == NULL` 전제. 잘못된 입력·할당 실패는 -1, 출력 NULL 유지. live 출력 거절은 무변경. 유효 요청 수락은 0과 핸들이며 연결 성공·실행 허가가 아님 |
| metadata/acquire 실패 | 오류를 기록한 CLOSED 객체. snapshot 확인 뒤 release |
| bind 실패 | 오류를 기록한 DISPOSING 객체. caller dispose만 반복 |
| PREPARED | child 미실행. 첫 step은 고정 observed start만 한 번 수행. 동기 실패는 DISPOSING이며 실패한 supervisor 필드는 읽지 않음 |
| PREPARED stop | stop은 latch만 설정. 다음 step에서 spawn 없이 DISPOSING으로 전환하고 dispose를 한 번 수행할 수 있음 |
| RUNNING | start 성공 뒤 caller adapter/peer 별칭을 NULL/zero로 버림. supervisor가 유일한 하위 소유자. 비동기 exec 실패도 caller dispose 금지 |
| RUNNING stop | latch 뒤 다음 적법한 START/CONNECT/SESSION step에서 supervisor stop만 한 번 전달하고 반환. DRAIN/CANCEL/CLEANUP·오류는 중복 전달 없이 기존 step 진행 |
| DISPOSING | `ppp_usb_dispose_step()`만 사용. 오류와 release는 독립. WAIT에서는 자원과 객체를 유지하며 다시 step |
| `step()` | start·stop 전달·supervisor step·caller dispose 중 한 동작. 진행 0, 로컬 CLOSED 1, 잘못된 호출 -1. 운영 오류를 반환값과 혼동하지 않음 |
| `snapshot()` | 상태·local_done·최초 단계 오류·cleanup_error·stop latch, 유효한 경우에만 supervisor phase/error 복사. raw 데이터·개인 경로 없음 |
| `peer_done()` | supervisor가 실제 DRAIN일 때만 기존 명시적 완료 전달. EOF/reap/close로 합성하지 않음 |
| `release(**session)` | CLOSED에서만 참조/객체 해제 후 같은 소유 핸들 NULL. 이미 NULL인 핸들은 no-op. 로컬 완료 전 거절 |

request의 service/device/config·metadata backend·timeout은 start 반환 뒤 보관하지 않습니다. request timeout은 OUT 전용이고 IN timeout은 위 규칙으로 start가 계산합니다. 하위 owned USB session이 참조와 설명자 복사를 담당하며 budgets와 capacity만 값으로 복사합니다. NULL timeout/default metadata는 기존 acquire/preflight 함수 본문을 재사용합니다. 따라서 NULL metadata는 실제 호스트 조회이며 테스트에서는 항상 fake backend를 넘깁니다. 객체 자체의 owner RunLoop 참조는 CLOSED 뒤 release까지 유지합니다.

모든 handle API는 snapshot/release도 같은 pthread·CFRunLoop와 busy guard를 검사합니다. 잘못된 owner/loop·SDK 또는 RunLoop dispatch 재진입은 핸들·상태·snapshot 출력 무변경으로 거절합니다. signal handler나 동시 호출은 지원하지 않으며 blocking loop·재start·재bind·자동 재접속은 없습니다.

최초 오류는 metadata/acquire/bind/start/supervisor/dispose 단계의 고정 enum입니다. `cleanup_error`는 caller dispose의 sticky 결과와 기존 supervisor가 노출하는 close 오류만 보존합니다. 기존 supervisor의 최초 오류에 가려진 모든 후속 정리 오류를 새 계층이 관측한다고 보장하지 않습니다. 정상 START/CONNECT 취소의 예상 core CONTROL은 supervisor.error가 없으면 일반 세션 오류로 승격하지 않습니다. 오류 후에도 CLOSED까지 step해야 하며 timeout·Abort·close 실패는 강제 free 근거가 아닙니다.

### 세션 회귀 검사 범위와 한계

기존 `ppp_usb_test.c` 번역 단위에 같은 새 구현을 include합니다. 고정 start 함수명 치환은 이 include 구간에만 적용해 자체 fixture의 observed start로 위임합니다. bind/할당/초기 clock 실패 주입과 RunLoop retain/release 카운트도 test-only입니다. production override나 새 generic backend는 없습니다. metadata의 parent 검사까지 fake로 처리하며 실제 호스트 preflight는 호출하지 않습니다.

잘못된 입력·live 출력 보존, metadata 차단/조회 오류의 zero I/O, acquire/bind/start 동기 실패와 초기화되지 않은 supervisor 미접근, PREPARED cancel, 첫 step의 start-only, observed stdout+exec gate, async ENOENT, START/CONNECT/SESSION stop, 명시적 peer_done/비합성, WAIT·clock·close 오류, owner/loop/SDK dispatch 재진입(snapshot/release 포함), 참조 균형을 검사하도록 추가했습니다. 테스트는 제한된 정리 루프에서 CLOSED를 얻지 못하면 EXIT_FAILURE로 끝내며 live context를 CHECK 실패 뒤 reset으로 버리지 않습니다. 실제 SDK는 compile-only 타깃과 의존성만 추가했고 production 앱 목록·금지 API 가드 기대값은 유지했습니다. 2026-10-15 coordinator의 세 번째 전체 오프라인 검사(`build/ppp-session-20261015-3/verify.status`, `verify_exit=0`)에서 빌드·SDK 단독 컴파일·일반 15개·ASan/UBSan 15개 테스트와 CLI/API 가드가 통과했습니다. 모든 테스트는 `failures=0`이며 `ppp_usb_test`는 일반 167,485 checks, sanitizer 169,189 checks였습니다. 별도 검토에서 코드와 검사 로그를 확인했고 미해결 결함은 없었습니다.

위 계약은 로컬 소유권 정리만 뜻합니다. 설치본 동작·인증·실제 readiness·PPP 인터페이스 귀속/복원·후손 전체 종료·LCP·AT/PDP 복원은 미검증입니다. 상위 종료 교환 입증 계층이 없으면 peer_done을 임의로 보내 정상 DRAIN 성공을 만들지 않습니다. 실제 USB·CGDATA·pppd(`--version` 포함)·sudo·호스트 설정/로그 조회·네트워크 변경·launchd는 이번 범위 밖입니다. 과거 SIGPIPE 실패는 원인 미확정·후속 미재현이며 새 검사가 이를 설명하지 않습니다.

## 내부 실행기와 signal 소유권 계약

`ppp_runner.h/c`는 opaque owning runner와 `begin/slice/restore/release` 네 API만 추가합니다. 기존 session·USB·supervisor·child generic ABI, production 앱 링크·CLI·discovery는 유지합니다. 생성한 owner pthread·CFRunLoop에서 직렬 호출하며 runner busy guard와 process-static lease/busy guard를 둡니다. lease는 signal 복원 뒤에도 최종 release까지 유지합니다. 2026-10-16 개발자의 보완본 전체 오프라인 검사(`build/ppp-runner-20261016-2/verify.status`, `verify_exit=0`)에서 빌드·일반 15개·ASan/UBSan 15개 테스트와 CLI/API 가드가 통과했습니다. 모든 테스트는 `failures=0`이며 `ppp_usb_test`는 일반 176,373 checks, sanitizer 179,505 checks였습니다. checks는 입력 수나 coverage 수가 아닙니다. runner/session 실제 SDK 단독 compile-only는 첫 검사 `build/ppp-runner-20261016/verify.stdout` 2–3행에서 성공했습니다. 보완은 테스트만 변경했으므로 두 번째 검사는 해당 최신 SDK 오브젝트를 재사용했습니다. LOW 회귀 공백 보완 뒤 독립 코드·테스트·원 로그 검토에서 미해결 발견 사항은 없었습니다.

| API | 소유권·반환·다음 동작 |
| --- | --- |
| `begin(**session, **runner)` | 유효한 같은 owner 세션, NULL runner 출력과 할당까지 거절은 -1이고 핸들 무변경. 수락은 0, `*session=NULL`로 이전. signal 설치 실패도 runner를 반환하므로 slice/restore/release로 정리. CLOSED 입력은 설치 생략 |
| `slice(runner, stop_flag, wait_ms, status)` | status 필수. NULL stop_flag는 추가 요청 없음. NULL wait_ms는 본문에서 10ms, 명시 값은 1–50ms. session step≤1, sleep≤1, CLOSED sleep0. 0 진행, 1 로컬 CLOSED, -1 거절/출력 무변경. 운영 오류는 status에서 별도 확인 |
| `restore(runner, status)` | live면 먼저 stop latch. 남은 action을 SIGTERM→SIGINT 순으로 모두 복원 시도. 0 완료, 1 미복원 항목 남음, -1 무변경 거절. 자원 해제/step은 하지 않으며 WAIT 중 복원 후에도 slice 계속 |
| `release(**runner)` | 로컬 CLOSED와 미복원 bitmask 0에서만 session release 뒤 runner/lease 해제. 같은 핸들 NULL 갱신. NULL 핸들 반복 no-op, NULL 이중 포인터/owner·재진입/미완료는 -1 |

실행기 소유의 세션 별칭은 호출자가 버립니다. 무한 blocking convenience 함수나 일반 signal mask API는 없습니다. caller가 slice를 반복하고 미완료 WAIT의 핸들과 모든 자원을 보유해야 합니다. release 거절 뒤 reset/free로 소유권을 버리면 안 됩니다. 로컬 CLOSED는 session 오류가 없다는 뜻도 signal 복원·장치/네트워크 복원 성공이라는 뜻도 아닙니다.

### 설치·rollback·복원과 전제 조건

SIGINT/SIGTERM disposition은 프로세스 전체 상태입니다. 호출자는 모든 변경을 직렬화하고 observer 같은 다른 소유자와 겹치지 않게 해야 합니다. 다른 thread가 두 signal을 차단하고 owner에서 처리하도록 준비하는 것도 호출자 전제입니다. 실행기는 전제 위반을 전부 검출하거나 다중 thread 전달을 조율한다고 주장하지 않습니다.

handler는 static `volatile sig_atomic_t`에 signal 번호를 대입만 합니다. 객체·session·stack 포인터, SDK·CFRunLoop·할당·로그는 접근하지 않습니다. 두 signal이 handler를 중첩하지 않도록 action mask에 SIGINT/SIGTERM을 함께 넣고 `sa_flags=0`으로 SA_RESTART를 쓰지 않습니다. flag는 첫 설치 직전에 한 번만 초기화합니다. SIGINT→SIGTERM 설치 중 두 번째 실패면 첫 번째를 즉시 rollback합니다. 설치하지 않은 CLOSED runner는 이전 lease의 잔여 flag를 자기 signal로 관측하지 않습니다.

status의 session snapshot과 최초 runner 오류는 별개입니다. `install_errno[2]`, `rollback_errno[2]`, `restore_errno[2]`는 각 항목 최초 errno를, `wait_errno`는 최초 대기 errno를 보존합니다. 배열 순서는 SIGINT, SIGTERM입니다. `installed_actions`는 이번 runner가 설치한 항목, `unrestored_actions`는 아직 복원하지 못한 항목의 bitmask입니다. `PPP_RUNNER_SIGINT`/`PPP_RUNNER_SIGTERM` bit는 성공한 복원에서만 지웁니다. 한 항목이 실패해도 다른 항목은 시도하고 재시도는 남은 bit만 처리합니다. 과거 errno와 최초 오류는 성공한 재시도 뒤에도 지우지 않습니다.

live restore는 disposition을 바꾸기 전에 stop을 latch해 이후 시작을 막습니다. 세션은 해제하지 않으므로 callback WAIT는 계속 진행할 수 있습니다. 복원 뒤 signal은 원래 handler 또는 기본 action(종료 포함)을 따르며 runner가 추가 graceful stop을 보장하지 않습니다. 설치 전/복원 후 원래 handler 소유의 signal을 가로챈다고 주장하지 않습니다. release 전에는 다른 runner begin이 lease로 거절됩니다.

### 요청 관측·대기·완료 한계

slice는 static signal flag와 optional 외부 stop flag를 step 전, step 후, 대기 후 확인합니다. stop은 한 번만 전달합니다. 같은 thread의 짧은 `nanosleep`만 쓰며 adapter pump 바깥 CFRunLoop dispatch는 추가하지 않습니다. EINTR은 remaining 재수면 없이 요청 관측으로 돌아가고 다른 sleep 오류는 sticky runner 오류와 stop 요청으로 남깁니다. 오류가 나도 session을 버리지 않습니다.

signal을 관측하기 전에 시작한 동기 SDK/step을 취소하지 못합니다. session_start의 metadata/acquire/bind는 runner begin 이전이므로 보호 범위 밖입니다. 신호 수신 즉시 신규 OUT이 모두 멈춘다거나 대기/SDK/동기 start에 hard wall-clock 상한이 있다고 설명하지 않습니다. 반복 signal/외부 stop은 deadline·자식 signal·Abort·close·release를 늘리지 않습니다. timeout/Abort 오류/clock 오류는 quiet나 조기 해제 근거가 아닙니다.

runner는 ready·peer_done·EOF·reap·quiet를 합성하지 않습니다. peer_done을 입증할 상위 종료 계층은 없으며 runner API에도 주입 구멍을 두지 않습니다. SESSION stop 뒤 peer_done이 없는 DRAIN은 정상 종료 교환으로 바꾸지 않고 timeout 오류와 로컬 정리로 검사합니다. 설치본 인증·readiness·인터페이스 귀속/복원·후손 전체 종료·LCP·AT/PDP 복원은 그대로 미검증입니다.

### 회귀 검사 결과

기존 `tests/ppp_usb_test.c`의 같은 구현 include·fake SDK·fake metadata·자체 child fixture를 확장합니다. sigaction/nanosleep은 runner include 구간에서만 치환하며 할당·owner/loop·session step/stop 카운트도 test-only seam입니다. production 주입 ABI는 없습니다. 실제 SDK는 runner source 단독 compile-only 의존성으로 포함하여 테스트 번역 단위의 우연한 include 의존성에 가리지 않게 합니다. 기존 suite 구조·CLI/API guard 기대값과 production 링크는 바꾸지 않습니다.

입력·할당·중첩/owner/다른 RunLoop/재진입 거절의 핸들·출력·SDK·signal 무변경, CLOSED 설치 생략과 이전 flag 비전파, 설치 1/2 실패·rollback 실패·복원 각 항목/양쪽 실패·역순/잔여 재시도·saved handler/mask/flags 보존을 검사하도록 작성했습니다. PREPARED 시작 전과 START/CONNECT/SESSION, 설치/대기/동기 OUT/SDK dispatch 경계 요청, handler의 session/SDK 무변경, 반복 stop과 deadline/signal 상한, 기본/범위 wait와 EINTR/기타 wait 오류, 실제 짧은 fixture cadence도 포함합니다.

지연 callback·Abort/clock/close 오류의 WAIT 보유·조기 release 거절, live restore stop 우선·lease 유지·재개 slice, inline Abort와 callback 전/후 outer SDK dispatch의 runner 재진입 거절, 최종 child reap·close/release·참조 균형을 검사하도록 추가했습니다. 테스트 정리 한계 내 CLOSED를 얻지 못하면 EXIT_FAILURE로 끝내며 live runner를 reset으로 버리지 않습니다. 설치 중 SIGINT와 첫 SIGTERM이 후속 signal 주입에 가려지던 LOW 회귀 공백을 보완해 첫 slice의 signal 번호·stop·spawn0을 각각 확인하고, 기존 반복 signal 멱등성 검사는 유지했습니다. 보완본 전체 검사에서 일반·ASan/UBSan 테스트가 모두 통과했습니다. 두 번째 검사 stderr에서 FAIL·ASan/UBSan·runtime·compiler·make 오류 패턴은 없었고, invalid status 파일 65개는 모두 exit 2였으며 마지막 WritePipe 가드까지 완료했습니다. 독립 코드·테스트·원 로그 검토에서 미해결 발견 사항은 없었습니다. 실제 USB open/송신·pppd(`--version` 포함)·host preflight·호스트 로그·sudo·네트워크·서비스 변경은 실행하지 않았습니다.

## 실제 child/pipe와 injected peer 계약

- `ppp_child_init/start/poll/read/write/signal/end_input/close`는 내부 로컬 transport API입니다. absolute path·argv·명시적인 NULL 종료 envp·absolute cwd를 받고 shell 없이 fork/execve합니다. 사용자 CLI에 임의 실행 옵션은 없습니다. pipe의 CLOEXEC 설정과 macOS `proc_pidinfo(PROC_PIDLISTFDS)` snapshot 뒤 child에서 stdio 및 exec handshake 외 fd를 닫습니다. 시작 동안 호출자는 프로세스 생성과 fd 생성/해제를 직렬화해야 합니다. post-fork 경로에서는 async-signal-safe 함수만 호출합니다.
- 별도 CLOEXEC pipe에서 exec/chdir 오류 번호를 받고, EOF를 보고된 exec 오류 없는 handshake 종료로 구분합니다. exec 전 종료도 EOF를 만들 수 있어 성공한 execve나 peer readiness의 증거가 아닙니다. child stdin/stdout/stderr 및 handshake의 부모 측 fd만 nonblocking입니다. partial 성공은 count만 반환하며 `INTR`/`AGAIN`은 진행하지 않습니다. write는 thread-local SIGPIPE mask를 사용하고, 기존 pending SIGPIPE와 전역 disposition은 보존합니다.
- `poll()`은 stderr를 호출당 최대 4096바이트 읽어 버립니다. 임시 버퍼는 1024바이트이고 진단 문자열·payload를 저장하지 않습니다. supervisor는 step당 이 poll을 한 번만 호출하고, 중복 readiness 확인과 signal 내부에서는 stderr를 읽지 않는 `poll_status()`를 씁니다. PID별 `waitpid(WNOHANG)`의 정상 결과만 `reaped`와 `reap_count`에 반영합니다. `ECHILD`만 `ownership_lost` 오류로 로컬 소유권을 끝내고 이후 신호를 금지합니다. 다른 wait 오류는 회수 증거가 아닙니다. stdout은 reap 뒤에도 읽어 보존하며 stdout/stderr EOF는 실제 `read()==0`으로만 기록합니다. 이미 관측한 stdout EOF의 반복 조회는 EOF지만 EOF 없이 닫은 FD의 read는 오류입니다. caller는 같은 PID를 별도로 reap하거나 SIGCHLD를 ignore/자동 reap으로 바꾸지 않습니다. API는 signal handler에서 호출하지 않습니다. close는 PID 소유 중에는 거부하고, 소유권 종료 뒤 fd를 한 번만 정리합니다. 실제 EOF 또는 handshake 완료 없이 닫으면 `transport_abandoned`로 남기고 EOF를 합성하지 않습니다. close(EINTR)를 같은 fd로 재시도하지 않습니다.
- `ppp_supervisor_start/ready/step/stop/peer_done/diagnostic`은 직렬 호출 API입니다. 성공한 start 뒤 peer close/release까지 객체와 context가 유효해야 합니다. 실패한 start는 peer 소유권을 가져가지 않습니다. 공개 fields는 관찰·stack 할당용이며 caller가 core fields를 수정하지 않습니다. 기존 core는 변경하지 않았습니다.
- 큐 용량은 각 1~4096바이트입니다. child 출력은 CONNECT 전에도 bounded pending-TX에 받고 공간이 없으면 실제 pipe에서 더 읽지 않습니다. 기존 시작 방식은 exec handshake와 injected ready를 기다립니다. 별도 `ppp_supervisor_start_observed()`는 필수 `begin_connect` hook을 요구하며 첫 성공 stdout read가 pending-TX에 수락된 뒤 관측 상태를 표시합니다. stderr·EOF·실패 read·exec만으로 ready를 만들지 않고 injected ready로 우회할 수 없습니다. marker나 PPP/LCP/frame/escape/FCS parser 없이 첫 바이트부터 모두 보존합니다. START→CONNECT 직전 clock과 child를 다시 확인하고 알려진 reap/EOF/error 또는 시작 예산 초과면 hook을 호출하지 않습니다. CONNECT 예산과 attempt를 호출 전에 고정하고 반환 뒤 clock/error를 검사합니다. hook 실패·늦은 성공·clock 오류에서는 read/새 submission/재송신 없이 cleanup하며 pending callback pump는 유지합니다. 생존 확인 이후 종료 경쟁까지 막는다는 뜻은 아닙니다. 그 뒤 USB read의 정확한 `CONNECT\r\n`과 tail offset은 core가 처리합니다. gate 이전 PPP 바이트를 USB에 쓰지 않습니다. pending-TX는 새 child read보다 먼저 core로 옮겨 바이트 순서를 보존합니다.
- USB read 성공은 1..4096바이트만 받습니다. 수락하지 못한 suffix를 supervisor의 고정 버퍼에 보존하고 다음 read 전에 먼저 resubmit합니다. injected read의 `AGAIN`은 **완료된 전송이 없다는 이벤트**이며 실제 failed USB completion을 감추는 수단이 아닙니다. 실제 failed completion은 ERROR/INTR/EOF로 주입하고 count/buffer를 무시합니다. USB write는 성공 전체 길이만 인정하며 어떤 실패·partial도 재송신하지 않습니다.
- 시작·CONNECT·세션·drain은 각각 양수 예산이며 nondecreasing monotonic clock과 덧셈 overflow를 검사합니다. callback 뒤에도 시계를 검사하고 성공 전달량은 기록합니다. blocking callback, frozen fake clock, kill 뒤 reap 지연의 wall-clock 상한은 보장하지 않습니다. caller는 cleanup 뒤에도 CLOSED까지 step을 계속 호출합니다.
- START/CONNECT의 stop은 `PPP_CANCEL`과 명시적 취소 latch를 먼저 설정해 begin_connect·USB read/write·readiness 재진입을 막습니다. CONTROL_WAIT이고 기존 core 오류가 없을 때만 기존 `offline_stream_stop()`의 정확한 INVALID/CONTROL/recovery_unknown 전이를 예상 취소로 구분합니다. 가짜 I/O 실패나 core 필드 변경으로 정리하지 않습니다. 이미 보낸 CGDATA를 취소·복구했다고 주장하지 않습니다. 직접 child에 TERM을 최대 한 번 요청하고 `drain_ms` 안에서 child 회수·exec 오류 해소·stdout/stderr 실제 EOF·peer quiet를 각각 기다립니다. exit 0 또는 `kill(SIGTERM)` 실제 성공 카운트 이후 SIGTERM 종료만 허용하며 exec/I/O 오류·ownership_lost·다른 종료를 숨기지 않습니다. pre-exec 신호도 exec pipe EOF를 만들 수 있으므로 exec_done은 exec 성공 증거가 아닙니다. return 0은 취소 요청 수락이지 PPP 성공이 아닙니다. peer_done·drain_complete를 합성하지 않고 CONTROL과 recovery unknown을 보존합니다. 시간 초과는 TIMEOUT과 기존 KILL fallback으로 전환합니다. 반복 stop은 기한·signal·quiesce 횟수를 늘리지 않습니다. 진단은 cancellation과 core-error를 표시하며 supervisor 오류가 없어도 core.recovery_unknown이면 recovery=unknown입니다. LCP·AT/PDP 복원·PPP 인터페이스 정리 성공으로 해석하지 않습니다.
- SESSION stop은 SIGTERM을 한 번 보내고 raw 종료 교환 forwarding을 유지합니다. 반복 stop은 deadline과 signal 횟수를 바꾸지 않습니다. caller/peer는 새 application source를 멈추며, supervisor는 payload에서 LCP 또는 `OK`를 찾지 않습니다. stdout EOF와 child exit는 별도 사건이며 미전달 TX를 먼저 drain합니다. EOF 뒤에도 peer RX는 명시적인 `peer_done()`까지 계속 처리합니다. peer_done 이후 새 USB read는 금지하고 기존 suffix/RX를 전달한 뒤 child stdin을 닫습니다. 큐가 비어도 peer_done·stdout EOF·PID reap 없이 drain 완료로 처리하지 않습니다.
- 정상 cleanup은 기존 DRAIN 전체 기한 안에서 미전달 큐 없음·stdin close·직접 자식 정상 회수와 exit 0·stdout/stderr 실제 EOF·exec handshake 해결·peer quiet를 모두 기다립니다. cleanup 진입으로 drain 기한을 연장하지 않습니다. quiesce/poll_quiet 오류와 callback 경과 시간을 먼저 반영하고 모든 장벽 뒤에만 `offline_stream_drain_complete()`를 호출합니다. `drain_done` 뒤 core는 ERROR 입력도 거절하므로 먼저 완료로 확정하지 않습니다. 순수 core API는 바꾸지 않습니다.
- 오류·timeout은 core의 기존 failure 이벤트로 recovery unknown을 고정하고 새 peer read/write/begin_connect를 중단합니다. 정상 cleanup 진입과 별개로 최초 오류 cleanup에서만 monotonic clock과 기존 `drain_ms`로 transport 기한을 정합니다. 정상 cleanup에서 늦게 실패해도 이 일회 전환을 실행하며 아직 소유한 PID에는 SIGKILL을 한 번 요청합니다. 반복 step/stop은 기한을 연장하지 않습니다. stdin을 닫고 stdout/stderr를 제한된 양만 읽어 버리며 실제 EOF를 계속 관측합니다. stdout은 정상 수락과 같은 step의 오류 폐기가 남은 4096바이트 예산을 공유하고, stderr는 별도 step당 4096바이트 상한입니다. 기한 이후 남은 로컬 FD를 폐기할 수 있지만 `transport_abandoned`이지 EOF가 아닙니다. clock 실패·역행·기한 overflow는 시간 대기만 중단하고 최초 오류를 보존합니다.
- quiesce/poll_quiet optional hooks는 함께 제공하거나 함께 NULL이며 NULL pair는 outstanding 비동기 작업 없음이라는 계약입니다. quiesce는 한 번 요청하고 매 step에서 child 상태와 이미 발행한 callback 반환을 독립적으로 진행합니다. Abort와 경합한 양수 count 성공 IN이나 quiesce 전에 완료 보관된 성공 IN이 미전달이면 `poll_quiet.error`로 보고합니다. callback quiet와 오류는 독립이며 새 I/O 없이 drain 완료 전에 오류 cleanup으로 전환합니다. 예상한 aborted completion은 취소 정상으로 유지하고 count 0/과대 성공은 기존대로 오류입니다. 직접 자식이 미회수이면 WAIT이며, 명시적 ECHILD 소유권 상실도 정상 회수가 아닌 오류입니다. timeout·signal 반환·Abort 반환·clock 오류는 소유권 종료 또는 peer quiet를 대신하지 않습니다. 이 두 장벽 뒤에만 core join/close/release를 수행합니다. 오류 CLOSED는 로컬 자원 정리 완료일 뿐 성공이 아닙니다. 진단은 `descendants=unproven`을 고정하며 정상 CLOSED나 EOF도 후손 전체 종료·LCP 종료·AT/PDP 복원 증거가 아닙니다.
- 공개 pin의 `notty`가 별도 charshunt를 만드는 점 때문에 직접 PID 수명과 transport 수명을 나눕니다. process group/session 격리·음수 PID 신호·WNOWAIT·group-ready handshake는 이번에 구현하지 않으며 charshunt 세션 탈출 여부도 미확인입니다. 설치본 1029에 대응하는 소스는 이번 공식 GitHub 조회에서 찾지 못했습니다. `nolog`는 시스템 로그까지 막지 않습니다. argv 전 사용자 설정과 argv 뒤 postoptions, 접근하지 못한 root `.ppprc` 등 기존 실행 전 제약을 유지합니다.

### 실제 로컬 fixture 검사와 한계

`tests/ppp_child_fixture.c`만 spawn합니다. 기존 테스트는 실제 pipe fd로 명시적 `RDY` handshake를 읽고 fake ready 이벤트를 넣으며 poll과 monotonic test deadline을 사용합니다. 출력 관측 검사는 marker 없는 binary stdout, stderr-only, silent timeout, exec 실패, 분할 출력, 원형·순서 보존, CONNECT+tail, hook 1회/실패/시간 초과/clock 후 no-read/no-write, 기존 NULL hook 호환을 확인합니다. 실제 `/usr/sbin/pppd`나 `--version`은 실행하지 않습니다. 고정 sleep으로 준비를 판정하지 않습니다. split echo·early TX·CONNECT+tail·작은 큐/suffix·실제 pipe backpressure·EOF/EPIPE/SIGPIPE·exec/chdir 실패·정상/비정상 exit·진단 burst·cancel 두 번·fd close/reap 한 번·fd 수량 복귀·private marker 비노출을 검사합니다. `make test-sanitize`에서는 fixture와 caller 모두 ASan/UBSan을 적용합니다. `make verify-offline`은 전체 빌드·실제 SDK 컴파일·일반 테스트·sanitizer 테스트와 CLI·API 가드를 함께 검사합니다. fake USB/clock 검사이므로 실제 장치, `pppd` readiness, IOKit 동시성, LCP, 인증·IP·인터넷·AT/PDP 복원 검증은 아닙니다.

fork pipeholder fixture의 부모 생존 모드는 holder가 stdio와 watchdog을 준비한 내부 handshake를 받은 뒤 `RDY`를 내보냅니다. stdin EOF는 제어 pipe close로 이어지고 main이 자기 직접 자식 holder를 `waitpid`합니다. stdout만 닫고 stderr 유지, stdin만 유지, 큰 stdout/stderr와 TERM 무시를 검사합니다. 일반 holder는 제어 EOF 뒤 50ms 유한 hold이며 main과 holder 각각 독립 SIGALRM watchdog 8초가 있어 stdin 대기·막힌 write·TERM 무시와 무관하게 종료합니다. 실패 경로도 제어 입력을 닫습니다.

main 선종료 반례는 `holder-exit` 한 종류뿐입니다. holder 준비 handshake 후 테스트의 `X` 요청으로 main만 정상 종료하고 holder는 stdin EOF 뒤부터 2000ms 파이프를 유지합니다. main 정상 회수와 stdout AGAIN을 먼저 확인하고, fake clock을 오류용 transport 기한 직전/경계로 옮겨 hold 만료 전에 abandonment를 검사합니다. 실제 EOF는 별도로 보유한 복제 read FD에서만 관측하며 transport의 EOF 필드를 바꾸지 않습니다. main 회수 대기 상한은 1초, 실제 EOF 대기는 stdin close 시점부터 5초여서 8초 watchdog 만료를 성공으로 받아들이지 않습니다. 이 반례는 일시적 재부모화를 의도합니다. 테스트는 손자를 waitpid하지 않으며 holder wait status는 시스템 reaper가 수거합니다. EOF로 후손 전체 사망을 입증했다고 주장하지 않습니다. PID 탐색·손자 kill·kqueue·subreaper·group/session 격리는 사용하지 않습니다. fixture 검사는 설치된 pppd/shunt 실기 검증이 아닙니다.

로컬 매뉴얼의 SIGINT/SIGTERM graceful LCP 설명은 미래 실제 launcher의 근거일 뿐, 테스트 child 종료를 LCP 성공으로 해석하지 않습니다. 관측 당시 `/etc/ppp`는 비어 있고 `~/.ppprc`는 발견하지 못했지만 전체 설정/hook을 통제했다는 증거가 아닙니다. SDK async IN completion byte count는 성공 때만 사용할 수 있으나 `AbortPipe` 뒤 callback completion 보장은 확인하지 못했습니다. SDK writer 한 곳과 비동기 OUT 금지는 유지하고, IF2 어댑터의 비동기 IN·Abort·event source 세 호출점만 제한적으로 허용합니다.

## 연결 유지 모드 (`ppp_lte_up`)

- 범위: 단발 실행기와 별도 바이너리입니다. 단발·auth의 고정 정책, 출력, 정지 순서는 바꾸지 않습니다. `ppp_session_request`의 `launch`가 0(ONCE)이면 기존 `ppp_supervisor_start_fixed`와 유도 IN timeout을 그대로 씁니다. `PPP_LAUNCH_MODE_UP`은 `ppp_supervisor_start_up`(고정 `ppp_launch_prepare_up`)을 쓰고 명시 `read_timeout_ms`가 없으면 거부합니다.
- 고정 argv: `notty nodetach nopersist noauth noipdefault defaultroute usepeerdns noipv6 nolog refuse-pap refuse-chap refuse-mschap refuse-mschap-v2 refuse-eap noacsp serviceid <PPP_LTE_UP_SERVICE_ID> lcp-echo-interval 10 lcp-echo-failure 3`. 공개 pin 기준 `defaultroute`는 `sifdefaultroute`로 IPv4 `OverridePrimary`와 IPv6 더미 primary를 게시하고 커널 경로를 직접 바꾸지 않습니다. primary 교체는 configd가 하며 첫 실기에서 확인합니다. `usepeerdns`는 서비스 DNS를 게시합니다.
- 예산: 시작 5초, CONNECT 10초, session 30일, DRAIN 10초, USB IN 75초. LCP echo 실패 감지 10초×4=40초가 IN timeout보다 짧다는 관계를 `_Static_assert`로 고정합니다. 앱 상한은 session−10분이며 TERM 경로로 멈춥니다.
- 연결 중 관측: observer 귀속(고정 ID의 PPP 키와 일치해야 함), `ppp_lte_link`의 주소 상태(absent/settling/stable/ambiguous/error, 3초 안정), 읽기 전용 primary probe(Global IPv4/IPv6 PrimaryService, PrimaryInterface, 서비스 DNS와 Global DNS 일치). 연결 전 250ms, 연결 뒤 1초마다 관측합니다. slice 대기는 직전에 바이트가 오갔으면 1ms, 아니면 10ms입니다.
- SC 정리(`ppp_lte_service`): 고정 ID의 상수 키 7개만 `SCDynamicStoreRemoveValue`로 지웁니다. 키 목록 패턴은 `^…[^/]+$`로 고정 ID 바로 아래만 읽고, 알 수 없는 entity는 세기만 합니다. 살아 있는 `pppd`(경로 또는 이름), 다른 `ppp_lte_up`, 읽지 못한 프로세스 목록·경로가 있으면 지우지 않습니다. 자기 자신의 pid는 제외합니다. 확인과 삭제 사이의 짧은 TOCTOU 창은 남습니다.
- 정리 시점: 시작 전(observer baseline 전), pppd reap 직후(CLOSED 전), 종료 직전(reap되었거나 시작되지 않은 경우), `--cleanup`. `ownership_lost`처럼 pppd가 살아 있을 수 있으면 지우지 않고 `next-run-cleanup=required`로 남깁니다.
- 가드: `verify-offline`은 `SCDynamicStoreRemoveValue`가 `ppp_lte_service.c`의 상수 키 호출 한 곳뿐인지, SC 쓰기·SCPreferences·SCNetworkSet API가 src에 없는지, `defaultroute|usepeerdns` 단어가 `ppp_launch.c`에만 있는지, `PPP_LAUNCH_MODE_UP`이 session과 up 실행기에만 있는지 확인합니다.
- 미확인: configd가 Setup: 항목 없는 State 전용 서비스를 primary로 올리는지, DNS 채택, IPv6 차단, 설치본 pppd와 공개 pin의 동일성, 모뎀의 LCP echo 응답, 처리량, 슬립·깨우기 뒤 동작, 원격 LCP·PDP 종료, 후손 프로세스.

## PPP 중계 처리량 (2026-10-18)

- 첫 데몬 실기에서 하향 1.6 / 상향 8 Mbps가 나왔습니다. 진단 라벨(`usb-in-bytes`, `usb-in-reads`, `usb-in-empty-polls`, `usb-validate-ms`, `usb-write-ms`, `loop-slices`)로 원인을 나눴습니다.
- 대기: 바이트가 움직이면 다음 100 slice 동안 1ms 대기를 유지합니다(이전에는 1ms·10ms를 번갈아 써서 읽기 경로가 쉬었습니다).
- 검사 재사용: 전체 레지스트리 검사가 회당 약 1.4ms였고 PPP 읽기·쓰기마다 돌아 루프가 초당 약 165회에 묶였습니다. 이제 PPP 읽기 제출과 쓰기는 성공한 검사를 250ms 동안 재사용합니다(`adapter_validate`, `CLOCK_MONOTONIC_RAW`). 검사 없는 쓰기(`usb_session_write_validated`)는 세션이 마지막으로 검증한 OUT 파이프만 받습니다. CGDATA 연결 명령과 AT 조회는 매번 검사합니다. 받아들인 위험: 창 안에서 생긴 다른 user client는 최대 250ms 늦게 보입니다. 그동안에도 독점 `USBInterfaceOpen` 때문에 그 client는 송수신하지 못하고, 분리·재열거는 열린 핸들의 오류로 바로 드러납니다. 정지(`quiesce`)할 때 캐시를 지웁니다.
- 결과: 하향 1.9 / 상향 11.6 Mbps, 유휴 지연 약 120ms, 루프 초당 약 258회. 남은 병목은 USB IN 읽기가 한 번에 하나라 "제출 → 빈 확인 → 수거"에 약 3 slice가 드는 구조와 slice당 약 3.9ms입니다. 다음 단계는 IN 읽기 여러 개 동시 대기와 이벤트 기반 대기입니다.
- 우선순위: launchd `ProcessType=Interactive`로 바꾼 뒤 루프가 초당 약 710회로 늘고 하향 3.5 / 상향 13.5 Mbps, 유휴 지연 약 89ms가 됐습니다(시험 맥은 부하 평균이 높았습니다).
- 미리 읽기: PPP 어댑터가 bulk IN 요청을 슬롯 링(기본 `PPP_USB_IN_SLOTS_DEFAULT` 4, 최대 4)으로 미리 걸어 둡니다. 거두는 즉시 빈 슬롯을 다시 채우고, 데이터는 제출 순서로만 넘깁니다. 슬롯 1개는 이전 동작과 같습니다(기존 테스트가 1로 고정). 정지할 때 아직 넘기지 못한 완료는 버리고 `usb-in-discarded`로 셉니다. 각 요청의 75초 타임아웃은 제출 시점부터 흐르므로, 유휴 링크에서 가장 오래된 요청이 (슬롯-1)×10초 더 늙을 수 있습니다. 그래서 `ppp_lte_up.c`가 LCP echo 실패(40초) + 30초 < 75초를 컴파일 시점에 확인합니다.
- 결과: 하향 3.6 / 상향 11.2 Mbps, 유휴 지연 약 67ms. 읽기 4개를 걸어 둬도 빈 확인이 읽기당 약 1.7회라 데이터가 읽는 속도보다 느리게 들어옵니다. 남은 한계는 망이나 모뎀 PPP 모드로 보고, 같은 SIM의 휴대폰 비교가 다음 확인입니다.
- 휴대폰 비교(사용자 보고): 같은 SIM의 휴대폰이 더 빠릅니다. 요금제 속도 제한도 걸려 있지 않습니다. 남은 한계는 모뎀의 PPP 모드 쪽으로 봅니다.

## 다이얼 전 신호 조회 (`up-signal`)

- 세션마다 `AT+CGDATA`를 보내기 전에 인터페이스 2에서 `AT+CSQ`와 `AT+CESQ`(3GPP TS 27.007, 읽기 전용)를 한 번씩 보냅니다. 같은 인터페이스는 PPP가 시작되면 데이터 전용이 되므로 다이얼 전에만 물을 수 있습니다.
- `--probe-status`와 같은 `run_slots` 규칙을 따릅니다. 자기 echo 뒤 최종 OK가 와야 숫자를 인정하고, 확인되지 않은 슬롯이 나오면 뒤 명령을 보내지 않습니다. 시간 초과 뒤에는 닫기만 합니다. 응답 줄은 버리고 숫자만 `up-signal` 한 줄로 남깁니다.
- 변환: CSQ rssi 0~31 → -113+2n dBm, CESQ rsrp 0~97 → n-141 dBm(하한), rsrq 0~34 → -20+n/2 dB(하한), 99·255는 `unknown`. 0은 "그 값 이하"라는 뜻이라(rssi 0 = -113 이하, rsrp 0 = -140 미만, rsrq 0 = -19.5 미만) 찍힌 값보다 더 나쁠 수 있습니다. 결정에는 쓰지 않습니다.
- 숫자는 자기 echo 뒤 최종 OK로 확인된 응답에 자기 값 줄이 정확히 하나일 때만 인정합니다(`value_lines`).
- 조회가 실패해도 세션은 그대로 진행합니다(`up-signal=unproven`). 조회 중 SIGHUP·SIGINT·SIGTERM이 오면 다이얼하지 않고 `reason=user-stop`으로 끝냅니다.
- 이 슬롯이 추가돼 `--probe-status`의 "중단 없음" `stop_index`가 6에서 7(`STATUS_PROBE_SLOTS`)로 바뀌었습니다. 다른 조회(`--probe-status`·`--probe-pdp`·`--probe-data-support`) 응답에 `+CESQ:` 줄이 섞이면 이제 foreign-prefix(mismatch)로 판정합니다(이전 unlisted-prefix). 더 엄격해지는 쪽입니다.
- 남은 위험: 조회가 쓰기 뒤 응답을 다 읽지 못하고 끝나면(timeout·time-budget·read-cap) 늦은 응답이 USB IN에 남아 이어지는 CGDATA 단계가 실패할 수 있습니다. 잘못 연결되지는 않고(CONNECT가 아닌 줄은 무시, ERROR는 실패) 세션 실패 뒤 데몬이 다시 시도하며, 다음 조회가 echo 앞 줄을 건너뛰며 남은 응답을 읽어 냅니다. 명령 모드의 모뎀은 보통 수 ms 안에 답하므로 실기에서 `stop=`이 none이 아닌 경우가 잦은지 지켜봅니다.

## 상태 파일과 제어 소켓 (선택 설치 에이전트)

- 범위: `ppp_lte_upd`가 상태를 파일로 내보내고, 고정 명령 네 개를 받는 제어 소켓을 엽니다. 사용자 도구 `dji-dongle`(setuid 아님)이 둘을 씁니다. 세션 실행기 `ppp_lte_up`과 USB·pppd·SystemConfiguration 경로는 바꾸지 않습니다. 새 스레드 없이 기존 대기 slice(`upd_wait`, `upd_child` 읽기 루프)에서 소켓을 확인합니다.
- 실행 디렉터리 `/var/run/dji-cellular-dongle-mac`: `/var/run`은 root:daemon 0775라 그 아래 경로를 직접 다루지 않습니다. 데몬이 만들 때는 0700으로 만든 뒤 열린 descriptor로 root:wheel 0755로 바꿉니다. 이미 있으면 `open(O_DIRECTORY|O_NOFOLLOW)` 뒤 `fstat`으로 root 소유·디렉터리·group/other 쓰기 없음을 확인하고, 아니면 `upd=status-blocked`(상태)·`upd=control-blocked`(소켓)를 남기고 그 기능 없이 계속합니다. 확인한 descriptor를 계속 쥐고, 상태 파일과 소켓의 모든 생성·교체·삭제는 `openat`·`renameat`·`unlinkat`·`fstatat`·`fchownat`·`fchmodat`(AT_SYMLINK_NOFOLLOW)로 이 descriptor 기준으로만 합니다. 쥔 descriptor가 검사를 통과하지 못하거나 지워졌으면(링크 수 0) 다시 열고 다시 확인합니다.
- 상태 파일 `status.json`(0644): 같은 디렉터리의 `status.json.tmp`를 먼저 지우고(심어 둔 symlink도 따라가지 않고 지움) `O_CREAT|O_EXCL|O_NOFOLLOW`로 만들어 쓴 뒤 `renameat`으로 바꿉니다. fsync는 하지 않습니다.
- 쓰기 시점: 타이머가 아니라 상태가 바뀔 때만 씁니다. `updated`를 뺀 본문이 직전과 같으면 쓰지 않습니다. `updated`는 벽시계 unix 초이고 뒤로 가지 않습니다. 쓰기가 실패하면 `upd=status-error`를 한 번 남기고 2초마다 다시 씁니다. 정상 종료 때는 `stopping`을 쓴 뒤 파일을 지워, 멈춘 데몬의 `connected`가 남지 않게 합니다(강제 종료 때는 남을 수 있습니다).
- 형식: 한 줄 JSON, 고정 키, 고정 라벨과 숫자만 담습니다. `{"version":1,"state":…,"devices":N|-1,"paused":bool,"session_started":unix|null,"connected_since":unix|null,"retry_at":unix|null,"signal":{"quality":"ok|partial|unproven"|null,"rssi_dbm":N|null,"rsrp_dbm":N|null,"rsrq_db":N.N|null,"measured_at":unix|null},"last_end":{"result":"none|stopped|ended|cleanup-required|blocked","reason":"<label>"|null,"at":unix|null},"updated":unix|null}`. state는 `waiting|connecting|connected|retry-wait|replug-required|paused|stopping|blocked`입니다. devices는 -1(모름) 또는 0~99로 자릅니다. 아직 신호를 잰 적이 없으면 `quality`는 null입니다.
- 자식 줄 해석: `up-signal=`·`up=connected`·`up-result=`·`up=blocked ` 줄만 봅니다. 값은 ` key=`부터 다음 공백까지만 읽습니다. 라벨은 허용 목록, reason/gate는 `[a-z0-9-]{1,40}`, dBm은 `-?[0-9]{1,3}`, rsrq는 `-?[0-9]{1,2}.[0-9]`만 받고 나머지는 null입니다. 자식 글자를 그대로 옮기지 않습니다. 신호는 다음 세션의 `up-signal`이 바꿀 때까지 남습니다. 결과 줄 없이 끝난 세션은 데몬이 `ended`와 고정 이유(`spawn-error|killed|reap-error|signaled|no-result`)를 붙입니다. `--cleanup` 자식 줄은 상태에 쓰지 않습니다.
- 제어 소켓 `/var/run/dji-cellular-dongle-mac/control.sock`: 시작 때 확인한 실행 디렉터리 안에 만듭니다. 같은 이름이 root 소유 소켓이면(`fstatat`) 지우고 다시 만들고, 그 밖의 것이면 `upd=control-blocked`를 남기고 제어 없이 계속합니다. `bind`는 경로를 받으므로 단일 스레드 데몬이 `fchdir(디렉터리 fd)` 뒤 umask 077로 이름만 bind하고 원래 작업 디렉터리로 돌아갑니다. 그다음 디렉터리 fd 기준 fchownat/fchmodat로 root:admin 0660으로 바꿉니다. 디렉터리가 root:wheel 0755라 그 사이 이름을 바꿔치기할 수 없습니다. admin 그룹을 찾지 못하면 root:wheel 0600으로 두고 `upd=control-root-only`를 남깁니다. listen 소켓은 non-blocking이고, slice마다 최대 4개 연결을 받되 연결들이 함께 쓰는 읽기 시간은 200ms가 상한입니다.
- 피어 확인: `getpeereid`로 uid를 얻고 uid 0이면 root, `mbr_check_membership`으로 admin 그룹 구성원이면 admin입니다. 그 밖에는 `error=denied`와 `upd=control-denied`입니다(소켓 권한이 먼저 막지만 데몬도 따로 확인합니다).
- 프로토콜: 연결마다 한 줄(64바이트 이하, `\n` 끝, 200ms 안)을 읽고 한 줄로 답한 뒤 닫습니다. 명령은 `status`·`pause`·`resume`·`reconnect`뿐이고 대소문자·공백·뒤따르는 줄·NUL이 있으면 거부합니다. 응답은 고정 라벨입니다: 현재 JSON 한 줄(status), `ok`, `ok ending-session`(실행 중인 세션을 끝내는 pause), `ok already-paused`, `ok already-running`, `ok not-saved`, `error=denied|unknown-command|bad-request|stopping|paused|replug-required|no-device`. 받아들인 연결은 `upd=control command=<명령|invalid> uid-class=root|admin`으로 남기고 사용자 이름·uid는 남기지 않습니다.
- pause: 세션 자식이 있으면 정지 요청과 같은 SIGHUP 전달·60초 뒤 SIGKILL 경로로 끝내고(`upd=ending-session for=pause`), 자식이 끝나면 `paused`입니다. paused 동안 새 세션을 시작하지 않지만 시작 때의 정리(`--cleanup`)는 그대로 합니다. `/Library/Application Support/dji-cellular-dongle-mac/paused`(디렉터리 root:wheel 0755, 파일 root:wheel 0644)가 있으면 paused입니다. 디렉터리를 `O_DIRECTORY|O_NOFOLLOW`로 열어 fstat으로 확인한 뒤 그 fd 기준으로 읽고 씁니다. 시작 때 이 디렉터리나 파일을 검증하지 못하면(symlink, 다른 소유자, 다른 종류) 예상치 못한 과금을 막도록 paused로 시작하고 `upd=pause-file-blocked paused=yes`를 남깁니다. 저장에 실패하면 메모리에서만 멈추고 `ok not-saved`, `upd=pause-save-error`입니다.
- resume: paused를 풀고 backoff를 최소값, 연속 빠른 실패 수를 0으로 되돌린 뒤 대기를 끊어 동글이 있으면 바로 시작합니다. 필요한 재연결(replug-required)은 풀지 않습니다. 모뎀을 되살리는 것은 새로 꽂는 일뿐이기 때문입니다.
- reconnect: 세션이 있으면 같은 SIGHUP 경로로 끝내고(`upd=ending-session for=reconnect`) backoff 없이 바로 새 세션을 시작합니다(`upd=reconnecting`). 재시도 대기 중이면 대기를 끊고, 끊긴 대기는 backoff를 두 배로 늘리지 않습니다. paused·replug-required이거나 동글 수가 0으로 확인됐으면 거절합니다. 아직 세지 않은 시작 정리 중에는 받아들입니다.
- 늦은 요청: pause·reconnect가 왔을 때 자식이 이미 스스로 끝나 SIGHUP을 보내지 못했다면 요청으로 끝난 세션으로 보지 않습니다. 빠른 실패 계산과 backoff 증가는 평소대로 하고, 다음 대기만 끊습니다.
- 정지 우선: launchd 정지(SIGTERM)는 그대로 우선합니다. 정지 뒤에 온 pause·resume·reconnect는 `error=stopping`이고, pause로 이미 SIGHUP을 보낸 세션에 정지가 와도 SIGHUP을 두 번 보내지 않으며 같은 60초 kill 시한을 씁니다. 요청으로 끝난 세션은 빠른 실패로 세지 않습니다.
- `dji-dongle`: `status`는 상태 파일을 직접 읽습니다(root 소유 일반 파일만, 권한 불필요). 고정 키를 찾아 값마다 다시 검증하고, 하나라도 어긋나면 파일 전체를 거부합니다. `pause|resume|reconnect`는 소켓으로 한 줄을 보냅니다. 실행 중인 세션을 끝내는 pause는 `pausing (current session is ending; up to 60 s)`로 알립니다. EACCES면 `need an admin account (try: sudo dji-dongle <명령>)`, 소켓이 없으면 데몬 미설치·미실행을 알립니다. 모르는 응답은 출력하지 않습니다. `watch`는 1초 간격으로 `ppp*` point-to-point 인터페이스의 AF_LINK `if_data` 바이트 수를 두 번 읽어 속도를 냅니다. 32비트 카운터가 한 번 넘어가는 것은 처리하고, 한 구간에 2^31바이트 이상 뛰면(인터페이스 재생성) 속도 대신 `-`를 보여 줍니다. 상태 파일이 잠깐 없으면(데몬 재시작) 두 번까지 `state: unknown (no status; retrying)`을 보이고 세 번째에 끝냅니다. 주소는 읽지도 출력하지도 않습니다.
- 빌드·설치: `make agent`(`AGENT_TARGETS`, 나중에 메뉴 막대 앱을 붙일 자리), `sudo make install-agent`는 `/usr/local/bin/dji-dongle`(root:wheel 0755, `install -S`)만 설치합니다. `/usr/local/bin`이 이미 있으면 소유자·권한을 건드리지 않습니다(Intel 맥의 Homebrew가 소유). `sudo make uninstall`은 소켓, 상태 디렉터리, 저장된 pause도 지웁니다.
- 검사: `ppp_lte_upd_test`가 fake backend로 상태 변화(신호·연결 시간·정지·제거), pause·resume(SIGHUP, 세션 보류, 즉시 재시작), reconnect(세션 중·재시도 대기 중), 거절 요청(권한 없는 피어, 모르는 명령, 줄 미완성, 두 줄, 64바이트 초과), 저장된 pause로 시작, status·control 경로 차단, 쓰기 실패 재시도, replug 유지, 정지 우선, 엄격한 줄 해석과 정확한 JSON 형식을 확인합니다. `dji_dongle_test`는 fake 파일·소켓·카운터로 JSON 해석 경계, 사람용 출력, 응답·오류 문구, 사용법 오류, watch와 카운터 넘김·급증·잠깐 없는 상태 파일을 확인합니다. 두 테스트 모두 `build/` 아래 mkdtemp 트리에서 실제 파일·소켓으로 네이티브 함수를 확인합니다(root 없이 기대 소유자를 현재 사용자로 바꿈): symlink·group 쓰기 디렉터리 거부, tmp 이름의 symlink 미추종, 남은 소켓 교체, 소켓 이름의 일반 파일 거부, 같은 uid 피어의 왕복(비 root라 admin 구성원 분기), admin 그룹이 없을 때 0600과 거부, pause 파일 생성·삭제·symlink 거부, CLI의 소유자·크기 검사, EACCES 안내, 응답 시간 초과. `verify-offline`은 `control.sock` 이름이 `ppp_lte_upd.c`와 `dji_dongle.c`에만 있는지, 데몬에 경로 기반 unlink·rename·chown·chmod가 없는지, CLI에 주소 출력·외부 실행 API가 없는지 확인합니다.
- 남은 위험: 실행 디렉터리가 없을 때 `mkdir`과 `open` 사이에 daemon 그룹이 같은 이름을 만들 수 있지만, 그때는 만든 것으로 보지 않고 fstat 검사(root 소유·쓰기 권한)에서 거부됩니다. root 소유 실행·pause 디렉터리 검사는 root(또는 테스트의 현재 사용자)를 믿는 것을 전제로 합니다. 강제 종료 뒤에는 상태 파일이 마지막 상태로 남을 수 있고 CLI는 이를 알아챌 수 없습니다(다음 시작 때 덮어씁니다). admin 구성원 확인은 Open Directory 조회에 기대며, 조회가 느리면 그 slice가 길어집니다. 실제 설치·실기 동작은 확인하지 않았습니다.
