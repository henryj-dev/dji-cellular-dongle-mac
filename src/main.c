#include "descriptor.h"
#include "discovery.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *stream) {
    fputs("사용법: dji-lte-mac [--help | --fixture PATH | --observe [--analyze] | --wait-reconnect --observe --analyze | --probe-at | --probe-ati | --probe-status | --probe-pdp | --probe-data-support]\n"
          "  옵션 없음      IOKit 캐시 설명자 조회만 실행. 장치 열기/USB 전송 없음\n"
          "  --fixture PATH ASCII hex 파일을 오프라인으로 검사 (최대 4096바이트)\n"
          "  --observe      non-seize 인터페이스 열기와 BULK IN 읽기만 실행\n"
          "                 인터페이스당 2초, 읽기 전체 15초/64KiB, 최대 8회씩\n"
          "                 수신 큐를 소비하며 프로토콜은 미확정. 별도 검토 후 실행\n"
          "  --analyze      --observe 뒤에만 사용. 성공한 읽기마다 최대 4096바이트 분석\n"
          "                 메모리에서 형식 후보/통계만 출력. raw/문자열/식별값 출력 없음\n"
          "                 후보는 프로토콜·칩셋·LTE 지원 증거가 아님. 조각 결합 없음\n"
          "  --wait-reconnect --observe --analyze  이 순서로만 사용\n"
          "                 새 registry ID 1대를 최대 90초/100ms 간격으로 대기 후 한 번 분석\n"
          "                 대기 중 registry 조회만 수행. 기존 세대는 읽지 않음\n"
          "                 동기 kernel 메타데이터 시간은 절대 90초 보장 밖. 실장치 검증 대기\n"
          "  --probe-at     단독 사용. 인터페이스 2 BULK OUT에 고정 3바이트 AT\\r 1회만 송신 후 IN 최대 4회/2초 분류\n"
          "  --probe-ati    단독 사용. 인터페이스 2에 고정 4바이트 ATI\\r 1회만 송신. 허용 문자 줄만 출력, 8자리+ 숫자 가림\n"
          "  --probe-status 단독 사용. 인터페이스 2에 AT+CPIN? AT+CSQ AT+COPS? AT+CEREG? 각 1회. 정한 필드만 출력, TAC/CI 미출력\n"
          "  --probe-pdp    단독 사용. 인터페이스 2에 AT+CGDCONT? 1회. cid/PDP 형식/APN만 출력, 주소는 유무·길이만\n"
          "  --probe-data-support 단독 사용. 인터페이스 2에 고정 12바이트 AT+CGDATA=?\\r 1회. 알려진 지원 목록만, 실제 PPP 접속 증거 아님\n"
          "  --help         이 안내 표시\n"
          "--open 및 임의 USB read/write/control 옵션은 제공하지 않습니다.\n", stream);
}

int main(int argc, char **argv) {
    if (argc == 1) return discovery_inspect();
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return DJI_OK;
    }
    if (argc == 2 && strcmp(argv[1], "--observe") == 0) return discovery_observe(0);
    if (argc == 2 && strcmp(argv[1], "--probe-at") == 0) return discovery_probe_at();
    if (argc == 2 && strcmp(argv[1], "--probe-ati") == 0) return discovery_probe_ati();
    if (argc == 2 && strcmp(argv[1], "--probe-status") == 0) return discovery_probe_status();
    if (argc == 2 && strcmp(argv[1], "--probe-pdp") == 0) return discovery_probe_pdp();
    if (argc == 2 && strcmp(argv[1], "--probe-data-support") == 0) return discovery_probe_data_support();
    if (argc == 3 && strcmp(argv[1], "--observe") == 0 &&
        strcmp(argv[2], "--analyze") == 0) return discovery_observe(1);
    if (argc == 4 && strcmp(argv[1], "--wait-reconnect") == 0 &&
        strcmp(argv[2], "--observe") == 0 && strcmp(argv[3], "--analyze") == 0)
        return discovery_wait_reconnect();
    if (argc == 3 && strcmp(argv[1], "--fixture") == 0 &&
        strcmp(argv[2], "--analyze") != 0 && strcmp(argv[2], "--wait-reconnect") != 0 &&
        strcmp(argv[2], "--probe-at") != 0 && strcmp(argv[2], "--probe-ati") != 0 &&
        strcmp(argv[2], "--probe-status") != 0 && strcmp(argv[2], "--probe-pdp") != 0 &&
        strcmp(argv[2], "--probe-data-support") != 0) {
        struct descriptor_config parsed;
        const char *error = NULL;
        if (descriptor_load_fixture(argv[2], &parsed, &error) != 0) {
            fprintf(stderr, "fixture 검사 실패: %s\n", error);
            return DJI_INVALID_DESCRIPTOR;
        }
        fputs("mode=offline\n", stdout);
        descriptor_print(&parsed, stdout);
        return DJI_OK;
    }
    usage(stderr);
    return DJI_USAGE;
}
