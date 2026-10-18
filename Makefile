CC := clang
CPPFLAGS := -Isrc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O2
FRAMEWORKS := -framework IOKit -framework CoreFoundation
SANITIZERS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
FIXTURE := tests/fixtures/dji-2ca3-4009-config.hex
OFFLINE_LOG_DIR ?= build
# Install layout. The daemon spawns the session binary from this fixed path.
HELPERDIR ?= /Library/PrivilegedHelperTools/dji-cellular-dongle-mac
UPD_DEFINES := -DUPD_CHILD_PATH='"$(HELPERDIR)/ppp_lte_up"'

.PHONY: all test test-sanitize verify-offline install install-files uninstall require-root clean-daemon agent install-agent uninstall-agent

all: build/dji-lte-mac build/ppp_lte_up build/ppp_lte_upd build/install/dji-cellular-dongle-mac.plist

build:
	mkdir -p build

build/dji-lte-mac: src/main.c src/descriptor.c src/descriptor.h src/discovery.c src/discovery.h src/usb_observer.c src/usb_observer.h src/usb_session.h src/observer_policy.c src/observer_policy.h src/payload_analysis.c src/payload_analysis.h src/reconnect.c src/reconnect.h src/at_probe.c src/at_probe.h src/status_probe.c src/status_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c src/descriptor.c src/discovery.c src/usb_observer.c src/observer_policy.c src/payload_analysis.c src/reconnect.c src/at_probe.c src/status_probe.c $(FRAMEWORKS) -o $@

build/descriptor_parser_test: tests/descriptor_parser_test.c src/descriptor.c src/descriptor.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/descriptor_parser_test.c src/descriptor.c -o $@

build/descriptor_parser_test_sanitize: tests/descriptor_parser_test.c src/descriptor.c src/descriptor.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/descriptor_parser_test.c src/descriptor.c -o $@

build/observer_test: tests/observer_test.c src/main.c src/discovery.h src/observer_policy.c src/observer_policy.h src/descriptor.c src/descriptor.h src/payload_analysis.c src/payload_analysis.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/observer_test.c src/observer_policy.c src/descriptor.c src/payload_analysis.c -o $@

build/observer_test_sanitize: tests/observer_test.c src/main.c src/discovery.h src/observer_policy.c src/observer_policy.h src/descriptor.c src/descriptor.h src/payload_analysis.c src/payload_analysis.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/observer_test.c src/observer_policy.c src/descriptor.c src/payload_analysis.c -o $@

build/payload_analysis_test: tests/payload_analysis_test.c src/payload_analysis.c src/payload_analysis.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/payload_analysis_test.c src/payload_analysis.c -o $@

build/payload_analysis_test_sanitize: tests/payload_analysis_test.c src/payload_analysis.c src/payload_analysis.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/payload_analysis_test.c src/payload_analysis.c -o $@

build/at_probe_test: tests/at_probe_test.c src/at_probe.c src/at_probe.h src/payload_analysis.c src/payload_analysis.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/at_probe_test.c src/at_probe.c src/payload_analysis.c -o $@

build/at_probe_test_sanitize: tests/at_probe_test.c src/at_probe.c src/at_probe.h src/payload_analysis.c src/payload_analysis.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/at_probe_test.c src/at_probe.c src/payload_analysis.c -o $@

build/ati_probe_test: tests/ati_probe_test.c src/at_probe.c src/at_probe.h src/payload_analysis.c src/payload_analysis.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ati_probe_test.c src/at_probe.c src/payload_analysis.c -o $@

build/ati_probe_test_sanitize: tests/ati_probe_test.c src/at_probe.c src/at_probe.h src/payload_analysis.c src/payload_analysis.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/ati_probe_test.c src/at_probe.c src/payload_analysis.c -o $@

build/status_probe_test: tests/status_probe_test.c src/status_probe.c src/status_probe.h src/at_probe.c src/at_probe.h src/payload_analysis.c src/payload_analysis.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/status_probe_test.c src/status_probe.c src/at_probe.c src/payload_analysis.c -o $@

build/status_probe_test_sanitize: tests/status_probe_test.c src/status_probe.c src/status_probe.h src/at_probe.c src/at_probe.h src/payload_analysis.c src/payload_analysis.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/status_probe_test.c src/status_probe.c src/at_probe.c src/payload_analysis.c -o $@

build/usb_recover_test: tests/usb_recover_test.c src/usb_recover.c src/usb_recover.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/usb_recover_test.c src/usb_recover.c $(FRAMEWORKS) -o $@

build/usb_recover_test_sanitize: tests/usb_recover_test.c src/usb_recover.c src/usb_recover.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/usb_recover_test.c src/usb_recover.c $(FRAMEWORKS) -o $@

build/pdp_probe_test: tests/pdp_probe_test.c src/status_probe.c src/status_probe.h src/at_probe.c src/at_probe.h src/payload_analysis.c src/payload_analysis.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/pdp_probe_test.c src/status_probe.c src/at_probe.c src/payload_analysis.c -o $@

build/pdp_probe_test_sanitize: tests/pdp_probe_test.c src/status_probe.c src/status_probe.h src/at_probe.c src/at_probe.h src/payload_analysis.c src/payload_analysis.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/pdp_probe_test.c src/status_probe.c src/at_probe.c src/payload_analysis.c -o $@

build/data_support_probe_test: tests/data_support_probe_test.c src/status_probe.c src/status_probe.h src/at_probe.c src/at_probe.h src/payload_analysis.c src/payload_analysis.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/data_support_probe_test.c src/status_probe.c src/at_probe.c src/payload_analysis.c -o $@

build/data_support_probe_test_sanitize: tests/data_support_probe_test.c src/status_probe.c src/status_probe.h src/at_probe.c src/at_probe.h src/payload_analysis.c src/payload_analysis.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/data_support_probe_test.c src/status_probe.c src/at_probe.c src/payload_analysis.c -o $@

build/reconnect_test: tests/reconnect_test.c src/reconnect.c src/reconnect.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/reconnect_test.c src/reconnect.c -o $@

build/reconnect_test_sanitize: tests/reconnect_test.c src/reconnect.c src/reconnect.h src/discovery.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/reconnect_test.c src/reconnect.c -o $@

build/observer_signal_test: tests/observer_signal_test.c src/usb_observer.c src/usb_observer.h src/usb_session.h src/observer_policy.c src/observer_policy.h src/discovery.h src/payload_analysis.c src/payload_analysis.h src/at_probe.c src/at_probe.h src/status_probe.c src/status_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/observer_signal_test.c src/observer_policy.c src/payload_analysis.c src/at_probe.c src/status_probe.c $(FRAMEWORKS) -o $@

build/observer_signal_test_sanitize: tests/observer_signal_test.c src/usb_observer.c src/usb_observer.h src/usb_session.h src/observer_policy.c src/observer_policy.h src/discovery.h src/payload_analysis.c src/payload_analysis.h src/at_probe.c src/at_probe.h src/status_probe.c src/status_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/observer_signal_test.c src/observer_policy.c src/payload_analysis.c src/at_probe.c src/status_probe.c $(FRAMEWORKS) -o $@

build/offline_stream_test: tests/offline_stream_test.c src/offline_stream.c src/offline_stream.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/offline_stream_test.c src/offline_stream.c -o $@

build/offline_stream_test_sanitize: tests/offline_stream_test.c src/offline_stream.c src/offline_stream.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/offline_stream_test.c src/offline_stream.c -o $@

build/ppp_launch_sdk.o: src/ppp_launch.c src/ppp_launch.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/ppp_launch.c -o $@

build/ppp_launch_test: tests/ppp_launch_test.c src/ppp_launch.c src/ppp_launch.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ppp_launch_test.c src/ppp_launch.c -o $@

build/ppp_launch_test_sanitize: tests/ppp_launch_test.c src/ppp_launch.c src/ppp_launch.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/ppp_launch_test.c src/ppp_launch.c -o $@

build/ppp_local_check: src/ppp_local_check.c src/ppp_launch.c src/ppp_launch.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/ppp_local_check.c src/ppp_launch.c -o $@

build/ppp_local_check_test: tests/ppp_local_check_test.c src/ppp_local_check.c src/ppp_launch.c src/ppp_launch.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ppp_local_check_test.c src/ppp_launch.c -o $@

build/ppp_local_check_test_sanitize: tests/ppp_local_check_test.c src/ppp_local_check.c src/ppp_launch.c src/ppp_launch.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/ppp_local_check_test.c src/ppp_launch.c -o $@

build/ppp_child_fixture: tests/ppp_child_fixture.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

build/ppp_child_fixture_sanitize: tests/ppp_child_fixture.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< -o $@

build/ppp_child_test: tests/ppp_child_test.c src/ppp_child.c src/ppp_child.h src/offline_stream.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ppp_child_test.c src/ppp_child.c -o $@

build/ppp_child_test_sanitize: tests/ppp_child_test.c src/ppp_child.c src/ppp_child.h src/offline_stream.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/ppp_child_test.c src/ppp_child.c -o $@

build/ppp_supervisor_test: tests/ppp_supervisor_test.c src/ppp_supervisor.c src/ppp_supervisor.h src/ppp_child.c src/ppp_child.h src/offline_stream.c src/offline_stream.h src/ppp_launch.c src/ppp_launch.h src/ppp_negotiation.c src/ppp_negotiation.h src/ppp_auth_probe.c src/ppp_auth_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ppp_supervisor_test.c src/ppp_supervisor.c src/ppp_child.c src/offline_stream.c src/ppp_launch.c src/ppp_negotiation.c src/ppp_auth_probe.c -o $@

build/ppp_supervisor_test_sanitize: tests/ppp_supervisor_test.c src/ppp_supervisor.c src/ppp_supervisor.h src/ppp_child.c src/ppp_child.h src/offline_stream.c src/offline_stream.h src/ppp_launch.c src/ppp_launch.h src/ppp_negotiation.c src/ppp_negotiation.h src/ppp_auth_probe.c src/ppp_auth_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/ppp_supervisor_test.c src/ppp_supervisor.c src/ppp_child.c src/offline_stream.c src/ppp_launch.c src/ppp_negotiation.c src/ppp_auth_probe.c -o $@

build/ppp_negotiation_test: tests/ppp_negotiation_test.c src/ppp_negotiation.c src/ppp_negotiation.h src/ppp_auth_probe.c src/ppp_auth_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ppp_negotiation_test.c src/ppp_negotiation.c src/ppp_auth_probe.c -o $@

build/ppp_negotiation_test_sanitize: tests/ppp_negotiation_test.c src/ppp_negotiation.c src/ppp_negotiation.h src/ppp_auth_probe.c src/ppp_auth_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/ppp_negotiation_test.c src/ppp_negotiation.c src/ppp_auth_probe.c -o $@

PPP_USB_SOURCES := src/ppp_runner.c src/ppp_session.c src/ppp_usb.c src/usb_observer.c src/observer_policy.c src/payload_analysis.c src/at_probe.c src/status_probe.c src/descriptor.c src/ppp_supervisor.c src/ppp_child.c src/offline_stream.c src/ppp_launch.c src/ppp_negotiation.c src/ppp_auth_probe.c
PPP_USB_SDK_HEADERS := src/ppp_runner.h src/ppp_session.h src/ppp_launch.h src/ppp_usb.h src/usb_session.h src/usb_observer.h src/descriptor.h src/ppp_supervisor.h src/ppp_child.h src/offline_stream.h src/discovery.h src/at_probe.h src/ppp_negotiation.h src/ppp_auth_probe.h
PPP_USB_HEADERS := $(PPP_USB_SDK_HEADERS) src/observer_policy.h src/payload_analysis.h src/at_probe.h src/status_probe.h

.PHONY: ppp-usb-sdk
ppp-usb-sdk: build/dji-lte-mac build/ppp_usb_sdk.o build/ppp_launch_sdk.o build/ppp_session_sdk.o build/ppp_runner_sdk.o build/ppp_negotiation_sdk.o

build/ppp_negotiation_sdk.o: src/ppp_negotiation.c src/ppp_negotiation.h src/ppp_auth_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/ppp_negotiation.c -o $@

build/ppp_runner_sdk.o: src/ppp_runner.c $(PPP_USB_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/ppp_runner.c -o $@

build/ppp_session_sdk.o: src/ppp_session.c $(PPP_USB_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/ppp_session.c -o $@

build/ppp_usb_sdk.o: src/ppp_usb.c $(PPP_USB_SDK_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/ppp_usb.c -o $@

build/ppp_usb_test: tests/ppp_usb_test.c $(PPP_USB_SOURCES) $(PPP_USB_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ppp_usb_test.c src/observer_policy.c src/payload_analysis.c src/at_probe.c src/status_probe.c src/descriptor.c src/ppp_supervisor.c src/ppp_child.c src/offline_stream.c src/ppp_launch.c src/ppp_negotiation.c src/ppp_auth_probe.c $(FRAMEWORKS) -o $@

build/ppp_usb_test_sanitize: tests/ppp_usb_test.c $(PPP_USB_SOURCES) $(PPP_USB_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/ppp_usb_test.c src/observer_policy.c src/payload_analysis.c src/at_probe.c src/status_probe.c src/descriptor.c src/ppp_supervisor.c src/ppp_child.c src/offline_stream.c src/ppp_launch.c src/ppp_negotiation.c src/ppp_auth_probe.c $(FRAMEWORKS) -o $@

test: build/ppp_lte_upd_test build/dji_dongle_test build/ppp_lte_up_test build/ppp_lte_up_integration_test build/ppp_lte_link_test build/ppp_lte_service_test build/ppp_lte_network_test build/ppp_lte_once_test build/ppp_lte_once_integration_test build/ppp_auth_probe_test build/ppp_auth_trial_test build/ppp_trial_observer_test build/ppp_local_check_test build/ppp_launch_test build/ppp_usb_test build/ppp_child_fixture build/ppp_child_test build/ppp_supervisor_test build/ppp_negotiation_test build/offline_stream_test build/descriptor_parser_test build/observer_test build/payload_analysis_test build/reconnect_test build/observer_signal_test build/at_probe_test build/ati_probe_test build/status_probe_test build/pdp_probe_test build/data_support_probe_test build/usb_recover_test
	./build/descriptor_parser_test $(FIXTURE)
	./build/observer_test $(FIXTURE)
	./build/payload_analysis_test
	./build/reconnect_test
	./build/observer_signal_test
	./build/at_probe_test
	./build/ati_probe_test
	./build/status_probe_test
	./build/usb_recover_test
	./build/pdp_probe_test
	./build/data_support_probe_test
	./build/offline_stream_test
	./build/ppp_negotiation_test
	./build/ppp_launch_test
	./build/ppp_local_check_test
	./build/ppp_auth_probe_test
	./build/ppp_auth_trial_test
	./build/ppp_lte_upd_test
	./build/dji_dongle_test
	./build/ppp_lte_up_test
	./build/ppp_lte_up_integration_test
	./build/ppp_lte_link_test
	./build/ppp_lte_service_test
	./build/ppp_lte_network_test
	./build/ppp_lte_once_test
	./build/ppp_lte_once_integration_test
	./build/ppp_trial_observer_test
	./build/ppp_child_test $(abspath build/ppp_child_fixture)
	./build/ppp_supervisor_test $(abspath build/ppp_child_fixture)
	./build/ppp_usb_test $(abspath build/ppp_child_fixture) $(FIXTURE)

test-sanitize: build/ppp_lte_upd_test_sanitize build/dji_dongle_test_sanitize build/ppp_lte_up_test_sanitize build/ppp_lte_up_integration_test_sanitize build/ppp_lte_link_test_sanitize build/ppp_lte_service_test_sanitize build/ppp_lte_network_test_sanitize build/ppp_lte_once_test_sanitize build/ppp_lte_once_integration_test_sanitize build/ppp_auth_probe_test_sanitize build/ppp_auth_trial_test_sanitize build/ppp_trial_observer_test_sanitize build/ppp_local_check_test_sanitize build/ppp_launch_test_sanitize build/ppp_usb_test_sanitize build/ppp_child_fixture_sanitize build/ppp_child_test_sanitize build/ppp_supervisor_test_sanitize build/ppp_negotiation_test_sanitize build/offline_stream_test_sanitize build/descriptor_parser_test_sanitize build/observer_test_sanitize build/payload_analysis_test_sanitize build/reconnect_test_sanitize build/observer_signal_test_sanitize build/at_probe_test_sanitize build/ati_probe_test_sanitize build/status_probe_test_sanitize build/pdp_probe_test_sanitize build/data_support_probe_test_sanitize build/usb_recover_test_sanitize
	./build/descriptor_parser_test_sanitize $(FIXTURE)
	./build/observer_test_sanitize $(FIXTURE)
	./build/payload_analysis_test_sanitize
	./build/reconnect_test_sanitize
	./build/observer_signal_test_sanitize
	./build/at_probe_test_sanitize
	./build/ati_probe_test_sanitize
	./build/status_probe_test_sanitize
	./build/usb_recover_test_sanitize
	./build/pdp_probe_test_sanitize
	./build/data_support_probe_test_sanitize
	./build/offline_stream_test_sanitize
	./build/ppp_negotiation_test_sanitize
	./build/ppp_launch_test_sanitize
	./build/ppp_local_check_test_sanitize
	./build/ppp_auth_probe_test_sanitize
	./build/ppp_auth_trial_test_sanitize
	./build/ppp_lte_upd_test_sanitize
	./build/dji_dongle_test_sanitize
	./build/ppp_lte_up_test_sanitize
	./build/ppp_lte_up_integration_test_sanitize
	./build/ppp_lte_link_test_sanitize
	./build/ppp_lte_service_test_sanitize
	./build/ppp_lte_network_test_sanitize
	./build/ppp_lte_once_test_sanitize
	./build/ppp_lte_once_integration_test_sanitize
	./build/ppp_trial_observer_test_sanitize
	./build/ppp_child_test_sanitize $(abspath build/ppp_child_fixture_sanitize)
	./build/ppp_supervisor_test_sanitize $(abspath build/ppp_child_fixture_sanitize)
	./build/ppp_usb_test_sanitize $(abspath build/ppp_child_fixture_sanitize) $(FIXTURE)

PPP_AUTH_SOURCES := src/ppp_auth_probe.c src/ppp_child.c src/ppp_launch.c src/ppp_trial_observer.c src/ppp_trial_preflight.c
PPP_AUTH_HEADERS := src/ppp_auth_probe.h src/ppp_child.h src/ppp_launch.h src/ppp_trial_observer.h src/offline_stream.h src/ppp_trial_preflight.h
PPP_AUTH_FRAMEWORKS := -framework CoreFoundation -framework SystemConfiguration

build/ppp_auth_trial: src/ppp_auth_trial.c $(PPP_AUTH_SOURCES) $(PPP_AUTH_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/ppp_auth_trial.c $(PPP_AUTH_SOURCES) $(PPP_AUTH_FRAMEWORKS) -o $@

build/ppp_auth_probe_test: tests/ppp_auth_probe_test.c src/ppp_auth_probe.c src/ppp_auth_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ppp_auth_probe_test.c src/ppp_auth_probe.c -o $@

build/ppp_auth_probe_test_sanitize: tests/ppp_auth_probe_test.c src/ppp_auth_probe.c src/ppp_auth_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/ppp_auth_probe_test.c src/ppp_auth_probe.c -o $@

build/ppp_auth_trial_test: tests/ppp_auth_trial_test.c src/ppp_auth_trial.c $(PPP_AUTH_SOURCES) $(PPP_AUTH_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ppp_auth_trial_test.c src/ppp_auth_probe.c src/ppp_child.c src/ppp_launch.c $(PPP_AUTH_FRAMEWORKS) -o $@

build/ppp_auth_trial_test_sanitize: tests/ppp_auth_trial_test.c src/ppp_auth_trial.c $(PPP_AUTH_SOURCES) $(PPP_AUTH_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/ppp_auth_trial_test.c src/ppp_auth_probe.c src/ppp_child.c src/ppp_launch.c $(PPP_AUTH_FRAMEWORKS) -o $@

build/ppp_trial_observer_test: tests/ppp_trial_observer_test.c src/ppp_trial_observer.c src/ppp_trial_observer.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/ppp_trial_observer_test.c $(PPP_AUTH_FRAMEWORKS) -o $@

build/ppp_trial_observer_test_sanitize: tests/ppp_trial_observer_test.c src/ppp_trial_observer.c src/ppp_trial_observer.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) tests/ppp_trial_observer_test.c $(PPP_AUTH_FRAMEWORKS) -o $@

.PHONY: ppp-auth-sdk
ppp-auth-sdk: build/ppp_auth_probe_sdk.o build/ppp_auth_trial_sdk.o build/ppp_trial_observer_sdk.o build/ppp_trial_preflight_sdk.o

build/ppp_auth_probe_sdk.o: src/ppp_auth_probe.c src/ppp_auth_probe.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/ppp_auth_trial_sdk.o: src/ppp_auth_trial.c $(PPP_AUTH_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/ppp_trial_observer_sdk.o: src/ppp_trial_observer.c src/ppp_trial_observer.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

PPP_LTE_HEADERS := src/ppp_lte_network.h src/discovery_internal.h $(PPP_USB_HEADERS) $(PPP_AUTH_HEADERS)
PPP_LTE_SOURCES := src/ppp_lte_once.c src/ppp_lte_network.c src/ppp_trial_preflight.c src/ppp_trial_observer.c src/discovery.c src/reconnect.c $(PPP_USB_SOURCES)

.PHONY: ppp-lte-sdk
ppp-lte-sdk: build/ppp_lte_once_sdk.o build/ppp_lte_network_sdk.o build/ppp_trial_preflight_sdk.o build/ppp_lte_service_sdk.o build/ppp_lte_link_sdk.o build/ppp_lte_up_sdk.o build/usb_recover_sdk.o

build/ppp_trial_preflight_sdk.o: src/ppp_trial_preflight.c $(PPP_AUTH_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/ppp_lte_once_sdk.o: src/ppp_lte_once.c $(PPP_LTE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/ppp_lte_network_sdk.o: src/ppp_lte_network.c $(PPP_LTE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/ppp_lte_once: $(PPP_LTE_SOURCES) $(PPP_LTE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PPP_LTE_SOURCES) $(FRAMEWORKS) -framework SystemConfiguration -o $@

build/ppp_lte_network_test: tests/ppp_lte_network_test.c src/ppp_lte_network.c $(PPP_LTE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(PPP_AUTH_FRAMEWORKS) -o $@

build/ppp_lte_network_test_sanitize: tests/ppp_lte_network_test.c src/ppp_lte_network.c $(PPP_LTE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< $(PPP_AUTH_FRAMEWORKS) -o $@

build/ppp_lte_once_test: tests/ppp_lte_once_test.c src/ppp_lte_once.c $(PPP_LTE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(FRAMEWORKS) -framework SystemConfiguration -o $@

build/ppp_lte_once_test_sanitize: tests/ppp_lte_once_test.c src/ppp_lte_once.c $(PPP_LTE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< $(FRAMEWORKS) -framework SystemConfiguration -o $@

build/ppp_lte_once_integration_test: tests/ppp_lte_once_integration_test.c src/ppp_lte_once.c src/ppp_lte_network.c $(PPP_LTE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(FRAMEWORKS) -framework SystemConfiguration -o $@

build/ppp_lte_once_integration_test_sanitize: tests/ppp_lte_once_integration_test.c src/ppp_lte_once.c src/ppp_lte_network.c $(PPP_LTE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< $(FRAMEWORKS) -framework SystemConfiguration -o $@

PPP_SERVICE_HEADERS := src/ppp_lte_service.h src/ppp_launch.h

build/ppp_lte_service_sdk.o: src/ppp_lte_service.c $(PPP_SERVICE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/ppp_lte_service_test: tests/ppp_lte_service_test.c src/ppp_lte_service.c $(PPP_SERVICE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(PPP_AUTH_FRAMEWORKS) -o $@

build/ppp_lte_service_test_sanitize: tests/ppp_lte_service_test.c src/ppp_lte_service.c $(PPP_SERVICE_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< $(PPP_AUTH_FRAMEWORKS) -o $@

PPP_LINK_HEADERS := src/ppp_lte_link.h src/ppp_lte_network.h src/ppp_trial_observer.h

build/ppp_lte_link_sdk.o: src/ppp_lte_link.c $(PPP_LINK_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/ppp_lte_link_test: tests/ppp_lte_link_test.c src/ppp_lte_link.c $(PPP_LINK_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(PPP_AUTH_FRAMEWORKS) -o $@

build/ppp_lte_link_test_sanitize: tests/ppp_lte_link_test.c src/ppp_lte_link.c $(PPP_LINK_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< $(PPP_AUTH_FRAMEWORKS) -o $@

PPP_UP_HEADERS := $(PPP_LTE_HEADERS) $(PPP_SERVICE_HEADERS) $(PPP_LINK_HEADERS) src/usb_recover.h
PPP_UP_SOURCES := src/ppp_lte_up.c src/usb_recover.c src/ppp_lte_link.c src/ppp_lte_service.c src/ppp_lte_network.c src/ppp_trial_preflight.c src/ppp_trial_observer.c src/discovery.c src/reconnect.c $(PPP_USB_SOURCES)

build/ppp_lte_up_sdk.o: src/ppp_lte_up.c $(PPP_UP_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/usb_recover_sdk.o: src/usb_recover.c src/usb_recover.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/ppp_lte_up: $(PPP_UP_SOURCES) $(PPP_UP_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PPP_UP_SOURCES) $(FRAMEWORKS) -framework SystemConfiguration -o $@

build/ppp_lte_up_test: tests/ppp_lte_up_test.c src/ppp_lte_up.c src/ppp_lte_link.c src/ppp_lte_service.c $(PPP_UP_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(FRAMEWORKS) -framework SystemConfiguration -o $@

build/ppp_lte_up_test_sanitize: tests/ppp_lte_up_test.c src/ppp_lte_up.c src/ppp_lte_link.c src/ppp_lte_service.c $(PPP_UP_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< $(FRAMEWORKS) -framework SystemConfiguration -o $@

build/ppp_lte_up_integration_test: tests/ppp_lte_up_integration_test.c src/ppp_lte_up.c src/ppp_lte_link.c src/ppp_lte_service.c $(PPP_UP_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(FRAMEWORKS) -framework SystemConfiguration -o $@

build/ppp_lte_up_integration_test_sanitize: tests/ppp_lte_up_integration_test.c src/ppp_lte_up.c src/ppp_lte_link.c src/ppp_lte_service.c $(PPP_UP_HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< $(FRAMEWORKS) -framework SystemConfiguration -o $@

# Rewritten only when HELPERDIR changes, so the daemon and plist follow it
# without rebuilding as root on every `sudo make install`.
build/helperdir.stamp: FORCE | build
	@printf '%s\n' '$(HELPERDIR)' | cmp -s - $@ || printf '%s\n' '$(HELPERDIR)' > $@

build/ppp_lte_upd: src/ppp_lte_upd.c build/helperdir.stamp | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(UPD_DEFINES) src/ppp_lte_upd.c $(FRAMEWORKS) -o $@

build/ppp_lte_upd_test: tests/ppp_lte_upd_test.c src/ppp_lte_upd.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(FRAMEWORKS) -o $@

build/ppp_lte_upd_test_sanitize: tests/ppp_lte_upd_test.c src/ppp_lte_upd.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< $(FRAMEWORKS) -o $@

# ---- Optional status/control agent (user side). See README "Status and control". ----
# AGENT_TARGETS is the hook for further agent pieces: append their build
# targets here and `make agent` builds them too.
AGENT_TARGETS := build/dji-dongle build/agent-app.stamp
AGENT_BINDIR ?= /usr/local/bin

agent: $(AGENT_TARGETS)

build/dji-dongle: src/dji_dongle.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

build/dji_dongle_test: tests/dji_dongle_test.c src/dji_dongle.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

build/dji_dongle_test_sanitize: tests/dji_dongle_test.c src/dji_dongle.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $< -o $@

# Menu bar app "build/DJI Dongle.app" (SwiftUI, swiftc only; no Xcode project).
# The stamp stands for the bundle because make targets cannot hold spaces.
# Core/ is pure logic plus the Darwin boundary; the test binary uses it alone.
SWIFTC := swiftc
# Built for the Mac it is built on (arm64 or x86_64), not universal.
APP_ARCH := $(shell uname -m)
SWIFT_FLAGS := -swift-version 6 -warnings-as-errors -target $(APP_ARCH)-apple-macos13 -O
APP_CORE_SOURCES := $(wildcard agent/app/Sources/Core/*.swift)
APP_UI_SOURCES := $(wildcard agent/app/Sources/UI/*.swift)
APP_STRINGS := agent/app/Resources/en.lproj/Localizable.strings agent/app/Resources/ko.lproj/Localizable.strings
APP_BUNDLE_ID := io.github.henryj-dev.dji-cellular-dongle-mac.agent

build/agent-app.stamp: $(APP_CORE_SOURCES) $(APP_UI_SOURCES) agent/app/Info.plist $(APP_STRINGS) | build
	rm -f $@
	rm -rf 'build/DJI Dongle.app'
	mkdir -p 'build/DJI Dongle.app/Contents/MacOS' 'build/DJI Dongle.app/Contents/Resources/en.lproj' 'build/DJI Dongle.app/Contents/Resources/ko.lproj'
	$(SWIFTC) $(SWIFT_FLAGS) -parse-as-library -module-name DJIDongle $(APP_CORE_SOURCES) $(APP_UI_SOURCES) -o 'build/DJI Dongle.app/Contents/MacOS/DJIDongle'
	plutil -lint agent/app/Info.plist $(APP_STRINGS) > /dev/null
	plutil -convert binary1 -o 'build/DJI Dongle.app/Contents/Info.plist' agent/app/Info.plist
	plutil -convert binary1 -o 'build/DJI Dongle.app/Contents/Resources/en.lproj/Localizable.strings' agent/app/Resources/en.lproj/Localizable.strings
	plutil -convert binary1 -o 'build/DJI Dongle.app/Contents/Resources/ko.lproj/Localizable.strings' agent/app/Resources/ko.lproj/Localizable.strings
	printf 'APPL????' > 'build/DJI Dongle.app/Contents/PkgInfo'
	codesign --force --sign - --timestamp=none 'build/DJI Dongle.app'
	touch $@

build/agent_app_test: $(APP_CORE_SOURCES) agent/app/Tests/main.swift | build
	$(SWIFTC) $(SWIFT_FLAGS) -module-name AgentAppTest $(APP_CORE_SOURCES) agent/app/Tests/main.swift -o $@

# ---- Install as a launchd daemon (root). See README "Install". ----
LAUNCHD_LABEL := io.github.henryj-dev.dji-cellular-dongle-mac
LAUNCHD_PLIST := /Library/LaunchDaemons/$(LAUNCHD_LABEL).plist
NEWSYSLOG_CONF := /etc/newsyslog.d/dji-cellular-dongle-mac.conf
# Created by the daemon at run time (status, control socket, saved pause).
UPD_STATUS_DIR := /var/run/dji-cellular-dongle-mac
UPD_PAUSE_DIR := /Library/Application Support/dji-cellular-dongle-mac
DESTDIR ?=
INSTALL_OWNER ?= -o root -g wheel

build/install/dji-cellular-dongle-mac.plist: dist/dji-cellular-dongle-mac.plist.in build/helperdir.stamp | build
	mkdir -p build/install
	sed 's|__HELPERDIR__|$(HELPERDIR)|' $< > $@
	plutil -lint $@ > /dev/null

# Never builds: run `make` as your user first. Only checks and copies, so a
# `sudo make install` cannot leave root-owned files in the build tree.
install-files:
	@for f in build/ppp_lte_up build/ppp_lte_upd build/install/dji-cellular-dongle-mac.plist; do \
	  test -f $$f || { echo "missing $$f: run 'make' (without sudo) first"; exit 1; }; done
	@grep -qF '$(HELPERDIR)/ppp_lte_up' build/ppp_lte_upd || { echo "build/ppp_lte_upd was built for another HELPERDIR; run: make clean-daemon && make HELPERDIR=$(HELPERDIR)"; exit 1; }
	@grep -qF '$(HELPERDIR)/ppp_lte_upd' build/install/dji-cellular-dongle-mac.plist || { echo "plist was built for another HELPERDIR; run: make clean-daemon && make HELPERDIR=$(HELPERDIR)"; exit 1; }
	@if [ -z '$(DESTDIR)' ]; then d='$(HELPERDIR)'; d=$${d%/*}; while [ -n "$$d" ]; do \
	  [ ! -L "$$d" ] && [ "$$(stat -f '%u' "$$d")" = 0 ] && [ $$(( 0$$(stat -f '%Lp' "$$d") & 022 )) = 0 ] || \
	  { echo "$$d must be root-owned and not group/other writable (the daemon refuses otherwise)"; exit 1; }; \
	  d=$${d%/*}; done; fi
	install -d $(INSTALL_OWNER) -m 0755 $(DESTDIR)$(HELPERDIR)
	install -S $(INSTALL_OWNER) -m 0755 build/ppp_lte_up build/ppp_lte_upd $(DESTDIR)$(HELPERDIR)/
	install -d $(INSTALL_OWNER) -m 0755 $(DESTDIR)/Library/LaunchDaemons $(DESTDIR)/etc/newsyslog.d $(DESTDIR)/var/log
	install -S $(INSTALL_OWNER) -m 0644 build/install/dji-cellular-dongle-mac.plist $(DESTDIR)$(LAUNCHD_PLIST)
	install -S $(INSTALL_OWNER) -m 0644 dist/dji-cellular-dongle-mac.newsyslog.conf $(DESTDIR)$(NEWSYSLOG_CONF)
	@test -e $(DESTDIR)/var/log/dji-cellular-dongle-mac.log || install $(INSTALL_OWNER) -m 0640 /dev/null $(DESTDIR)/var/log/dji-cellular-dongle-mac.log

require-root:
	@test "$$(id -u)" -eq 0 || { echo "run with sudo: sudo make $(MAKECMDGOALS)"; exit 1; }

# Stop the old daemon before replacing its files, then start the new one.
install: require-root
	-launchctl bootout system/$(LAUNCHD_LABEL) 2> /dev/null
	$(MAKE) install-files
	launchctl bootstrap system $(LAUNCHD_PLIST)
	@echo "dji-cellular-dongle-mac installed. While the dongle is plugged in, ALL of this Mac's traffic goes over LTE."
	@echo "Log: /var/log/dji-cellular-dongle-mac.log   Remove: sudo make uninstall (stopping can take up to 3 minutes)"

uninstall: require-root
	-launchctl bootout system/$(LAUNCHD_LABEL) 2> /dev/null
	@! launchctl print system/$(LAUNCHD_LABEL) > /dev/null 2>&1 || { echo "the daemon is still loaded; nothing removed. Try again."; exit 1; }
	-test -x $(HELPERDIR)/ppp_lte_up && $(HELPERDIR)/ppp_lte_up --cleanup
	rm -f $(LAUNCHD_PLIST) $(NEWSYSLOG_CONF) $(HELPERDIR)/ppp_lte_up $(HELPERDIR)/ppp_lte_upd
	-rmdir $(HELPERDIR)
	rm -f $(UPD_STATUS_DIR)/control.sock $(UPD_STATUS_DIR)/status.json $(UPD_STATUS_DIR)/status.json.tmp '$(UPD_PAUSE_DIR)/paused'
	-rmdir $(UPD_STATUS_DIR) '$(UPD_PAUSE_DIR)'
	@echo "dji-cellular-dongle-mac removed. The log /var/log/dji-cellular-dongle-mac.log is kept."

# The CLI and the menu bar app; the daemon (with status and control) comes
# from `make install`. Never builds, so no root-owned files land in build/.
# An existing app is replaced only if it is a real directory whose bundle id
# is ours; the path is a fixed literal under $(DESTDIR). The new copy is made
# and owned in a temporary name next to it, then moved into place.
install-agent: require-root
	@test -f build/dji-dongle || { echo "missing build/dji-dongle: run 'make agent' (without sudo) first"; exit 1; }
	@test -f build/agent-app.stamp && test -x 'build/DJI Dongle.app/Contents/MacOS/DJIDongle' || { echo "missing build/DJI Dongle.app: run 'make agent' (without sudo) first"; exit 1; }
	@# An existing bindir is left as is (Homebrew owns /usr/local/bin on Intel Macs).
	test -d $(DESTDIR)$(AGENT_BINDIR) || install -d $(INSTALL_OWNER) -m 0755 $(DESTDIR)$(AGENT_BINDIR)
	install -S $(INSTALL_OWNER) -m 0755 build/dji-dongle $(DESTDIR)$(AGENT_BINDIR)/dji-dongle
	test -d '$(DESTDIR)/Applications' || install -d $(INSTALL_OWNER) -m 0755 '$(DESTDIR)/Applications'
	@a='$(DESTDIR)/Applications/DJI Dongle.app'; t='$(DESTDIR)/Applications/.DJI Dongle.app.tmp.'$$$$; \
	if [ -L "$$a" ] || { [ -e "$$a" ] && [ ! -d "$$a" ]; }; then echo "$$a is not a plain app bundle; not touched"; exit 1; fi; \
	if [ -d "$$a" ]; then \
	  id=$$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$$a/Contents/Info.plist" 2> /dev/null); \
	  [ "$$id" = '$(APP_BUNDLE_ID)' ] || { echo "$$a belongs to another app; not touched"; exit 1; }; fi; \
	echo "ditto 'build/DJI Dongle.app' '$$t'"; \
	ditto 'build/DJI Dongle.app' "$$t" && chown -R root:wheel "$$t" && chmod -R go-w "$$t" || { rm -rf "$$t"; exit 1; }; \
	if [ -d "$$a" ] && [ ! -L "$$a" ]; then echo "rm -rf '$$a'"; rm -rf '$(DESTDIR)/Applications/DJI Dongle.app'; fi; \
	if [ -e "$$a" ] || [ -L "$$a" ]; then echo "$$a is in the way; not touched"; rm -rf "$$t"; exit 1; fi; \
	mv -h "$$t" "$$a" || { rm -rf "$$t"; exit 1; }; \
	if [ -L "$$a" ] || [ ! -d "$$a" ]; then echo "$$a is not the installed bundle"; exit 1; fi
	@echo "DJI Dongle.app installed in /Applications. Open it once; it lives in the menu bar."
	@echo "dji-dongle installed. Try: dji-dongle status   Remove: sudo make uninstall-agent"

uninstall-agent: require-root
	rm -f $(DESTDIR)$(AGENT_BINDIR)/dji-dongle
	@a='$(DESTDIR)/Applications/DJI Dongle.app'; \
	if [ -d "$$a" ] && [ ! -L "$$a" ]; then \
	  id=$$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$$a/Contents/Info.plist" 2> /dev/null); \
	  if [ "$$id" = '$(APP_BUNDLE_ID)' ]; then echo "rm -rf '$$a'"; rm -rf '$(DESTDIR)/Applications/DJI Dongle.app'; \
	  else echo "$$a belongs to another app; not touched"; fi; \
	elif [ -e "$$a" ] || [ -L "$$a" ]; then echo "$$a is not a plain app bundle; not touched"; fi

clean-daemon:
	rm -f build/ppp_lte_upd build/helperdir.stamp build/install/dji-cellular-dongle-mac.plist

FORCE:

# Full offline build, SDK, test, sanitizer and CLI/API checks;
# no real discovery, wait-reconnect, observation or probe calls.
verify-offline: all agent build/agent_app_test ppp-usb-sdk ppp-auth-sdk ppp-lte-sdk build/ppp_lte_once build/ppp_lte_up build/ppp_lte_upd build/ppp_auth_trial build/ppp_local_check test test-sanitize
	mkdir -p $(OFFLINE_LOG_DIR)
	./build/ppp_lte_once --help > $(OFFLINE_LOG_DIR)/lte-once-help.stdout
	@./build/ppp_lte_once --lte-once > $(OFFLINE_LOG_DIR)/lte-once-invalid.stdout 2> $(OFFLINE_LOG_DIR)/lte-once-invalid.stderr; rc=$$?; test "$$rc" -eq 2
	grep -q -- '--endpoint' $(OFFLINE_LOG_DIR)/lte-once-help.stdout
	./build/ppp_lte_up --help > $(OFFLINE_LOG_DIR)/lte-up-help.stdout
	grep -q -- '--up ' $(OFFLINE_LOG_DIR)/lte-up-help.stdout
	grep -q -- '--cleanup' $(OFFLINE_LOG_DIR)/lte-up-help.stdout
	@./build/ppp_lte_up --down > $(OFFLINE_LOG_DIR)/lte-up-invalid.stdout 2> $(OFFLINE_LOG_DIR)/lte-up-invalid.stderr; rc=$$?; printf 'lte_up_invalid_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/lte-up-invalid.status; test "$$rc" -eq 2
	@./build/ppp_lte_up --up extra > $(OFFLINE_LOG_DIR)/lte-up-extra.stdout 2> $(OFFLINE_LOG_DIR)/lte-up-extra.stderr; rc=$$?; test "$$rc" -eq 2
	@./build/ppp_lte_up > $(OFFLINE_LOG_DIR)/lte-up-none.stdout 2> $(OFFLINE_LOG_DIR)/lte-up-none.stderr; rc=$$?; test "$$rc" -eq 2
	! grep -nE 'TODO|FIXME|HACK|test\.(skip|only)|\.skip\(|\.only\(|placeholder' src/ppp_lte*.[ch] src/ppp_trial_preflight.[ch] tests/ppp_lte*_test.c src/ppp_negotiation.[ch] tests/ppp_negotiation_test.c
	./build/ppp_auth_trial --help > $(OFFLINE_LOG_DIR)/auth-trial-help.stdout
	@./build/ppp_auth_trial --check-host > $(OFFLINE_LOG_DIR)/auth-trial-invalid.stdout 2> $(OFFLINE_LOG_DIR)/auth-trial-invalid.stderr; rc=$$?; printf 'auth_trial_invalid_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/auth-trial-invalid.status; test "$$rc" -eq 2
	grep -q -- '--local-auth-once' $(OFFLINE_LOG_DIR)/auth-trial-help.stdout
	! grep -nE 'TODO|FIXME|HACK|test\.(skip|only)|\.skip\(|\.only\(|placeholder' src/ppp_auth_probe.[ch] src/ppp_auth_trial.c src/ppp_trial_observer.[ch] tests/ppp_auth_probe_test.c tests/ppp_auth_trial_test.c tests/ppp_trial_observer_test.c
	git diff --check
	./build/ppp_local_check --help > $(OFFLINE_LOG_DIR)/local-check-help.stdout
	@./build/ppp_local_check --run > $(OFFLINE_LOG_DIR)/local-check-invalid.stdout 2> $(OFFLINE_LOG_DIR)/local-check-invalid.stderr; rc=$$?; printf 'local_check_invalid_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/local-check-invalid.status; test "$$rc" -eq 2
	./build/dji-lte-mac --help > $(OFFLINE_LOG_DIR)/help.stdout
	./build/dji-lte-mac --fixture $(FIXTURE) > $(OFFLINE_LOG_DIR)/fixture.stdout
	@./build/dji-lte-mac --observe invalid > $(OFFLINE_LOG_DIR)/invalid-observe.stdout 2> $(OFFLINE_LOG_DIR)/invalid-observe.stderr; status=$$?; printf 'invalid_observe_exit=%s\n' "$$status" > $(OFFLINE_LOG_DIR)/invalid-observe.status; test "$$status" -eq 2
	@./build/dji-lte-mac --open > $(OFFLINE_LOG_DIR)/invalid-option.stdout 2> $(OFFLINE_LOG_DIR)/invalid-option.stderr; status=$$?; printf 'invalid_option_exit=%s\n' "$$status" > $(OFFLINE_LOG_DIR)/invalid-option.status; test "$$status" -eq 2
	@./build/dji-lte-mac --analyze > $(OFFLINE_LOG_DIR)/invalid-analyze.stdout 2> $(OFFLINE_LOG_DIR)/invalid-analyze.stderr; status=$$?; printf 'invalid_analyze_exit=%s\n' "$$status" > $(OFFLINE_LOG_DIR)/invalid-analyze.status; test "$$status" -eq 2
	@./build/dji-lte-mac --analyze --observe > $(OFFLINE_LOG_DIR)/invalid-order.stdout 2> $(OFFLINE_LOG_DIR)/invalid-order.stderr; status=$$?; printf 'invalid_order_exit=%s\n' "$$status" > $(OFFLINE_LOG_DIR)/invalid-order.status; test "$$status" -eq 2
	@./build/dji-lte-mac --observe --analyze extra > $(OFFLINE_LOG_DIR)/invalid-extra.stdout 2> $(OFFLINE_LOG_DIR)/invalid-extra.stderr; status=$$?; printf 'invalid_extra_exit=%s\n' "$$status" > $(OFFLINE_LOG_DIR)/invalid-extra.status; test "$$status" -eq 2
	@./build/dji-lte-mac --fixture $(FIXTURE) --analyze > $(OFFLINE_LOG_DIR)/invalid-fixture-analysis.stdout 2> $(OFFLINE_LOG_DIR)/invalid-fixture-analysis.stderr; status=$$?; printf 'invalid_fixture_analysis_exit=%s\n' "$$status" > $(OFFLINE_LOG_DIR)/invalid-fixture-analysis.status; test "$$status" -eq 2
	@./build/dji-lte-mac --help --analyze > $(OFFLINE_LOG_DIR)/invalid-help-analysis.stdout 2> $(OFFLINE_LOG_DIR)/invalid-help-analysis.stderr; status=$$?; printf 'invalid_help_analysis_exit=%s\n' "$$status" > $(OFFLINE_LOG_DIR)/invalid-help-analysis.status; test "$$status" -eq 2
	@./build/dji-lte-mac --fixture --analyze > $(OFFLINE_LOG_DIR)/invalid-fixture-token.stdout 2> $(OFFLINE_LOG_DIR)/invalid-fixture-token.stderr; status=$$?; printf 'invalid_fixture_token_exit=%s\n' "$$status" > $(OFFLINE_LOG_DIR)/invalid-fixture-token.status; test "$$status" -eq 2
	@./build/dji-lte-mac --wait-reconnect --observe > $(OFFLINE_LOG_DIR)/invalid-wait.stdout 2> $(OFFLINE_LOG_DIR)/invalid-wait.stderr; status=$$?; printf 'invalid_wait_exit=%s\n' "$$status" > $(OFFLINE_LOG_DIR)/invalid-wait.status; test "$$status" -eq 2
	@./build/dji-lte-mac --observe --analyze --wait-reconnect > $(OFFLINE_LOG_DIR)/invalid-wait-order.stdout 2> $(OFFLINE_LOG_DIR)/invalid-wait-order.stderr; status=$$?; printf 'invalid_wait_order_exit=%s\n' "$$status" > $(OFFLINE_LOG_DIR)/invalid-wait-order.status; test "$$status" -eq 2
	@./build/dji-lte-mac --probe-at extra > $(OFFLINE_LOG_DIR)/invalid-probe-extra.stdout 2> $(OFFLINE_LOG_DIR)/invalid-probe-extra.stderr; rc=$$?; printf 'invalid_probe_extra_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-probe-extra.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --observe --probe-at > $(OFFLINE_LOG_DIR)/invalid-observe-probe.stdout 2> $(OFFLINE_LOG_DIR)/invalid-observe-probe.stderr; rc=$$?; printf 'invalid_observe_probe_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-observe-probe.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-at --observe > $(OFFLINE_LOG_DIR)/invalid-probe-observe.stdout 2> $(OFFLINE_LOG_DIR)/invalid-probe-observe.stderr; rc=$$?; printf 'invalid_probe_observe_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-probe-observe.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-at --analyze > $(OFFLINE_LOG_DIR)/invalid-probe-analyze.stdout 2> $(OFFLINE_LOG_DIR)/invalid-probe-analyze.stderr; rc=$$?; printf 'invalid_probe_analyze_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-probe-analyze.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-at --probe-at > $(OFFLINE_LOG_DIR)/invalid-probe-twice.stdout 2> $(OFFLINE_LOG_DIR)/invalid-probe-twice.stderr; rc=$$?; printf 'invalid_probe_twice_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-probe-twice.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --wait-reconnect --probe-at > $(OFFLINE_LOG_DIR)/invalid-wait-probe.stdout 2> $(OFFLINE_LOG_DIR)/invalid-wait-probe.stderr; rc=$$?; printf 'invalid_wait_probe_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-wait-probe.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --help --probe-at > $(OFFLINE_LOG_DIR)/invalid-help-probe.stdout 2> $(OFFLINE_LOG_DIR)/invalid-help-probe.stderr; rc=$$?; printf 'invalid_help_probe_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-help-probe.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --fixture --probe-at > $(OFFLINE_LOG_DIR)/invalid-fixture-probe.stdout 2> $(OFFLINE_LOG_DIR)/invalid-fixture-probe.stderr; rc=$$?; printf 'invalid_fixture_probe_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-fixture-probe.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-ati extra > $(OFFLINE_LOG_DIR)/invalid-ati-extra.stdout 2> $(OFFLINE_LOG_DIR)/invalid-ati-extra.stderr; rc=$$?; printf 'invalid_ati_extra_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-ati-extra.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-at --probe-ati > $(OFFLINE_LOG_DIR)/invalid-at-ati.stdout 2> $(OFFLINE_LOG_DIR)/invalid-at-ati.stderr; rc=$$?; printf 'invalid_at_ati_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-at-ati.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-ati --probe-at > $(OFFLINE_LOG_DIR)/invalid-ati-at.stdout 2> $(OFFLINE_LOG_DIR)/invalid-ati-at.stderr; rc=$$?; printf 'invalid_ati_at_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-ati-at.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --observe --probe-ati > $(OFFLINE_LOG_DIR)/invalid-observe-ati.stdout 2> $(OFFLINE_LOG_DIR)/invalid-observe-ati.stderr; rc=$$?; printf 'invalid_observe_ati_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-observe-ati.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-ati --observe > $(OFFLINE_LOG_DIR)/invalid-ati-observe.stdout 2> $(OFFLINE_LOG_DIR)/invalid-ati-observe.stderr; rc=$$?; printf 'invalid_ati_observe_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-ati-observe.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-ati --analyze > $(OFFLINE_LOG_DIR)/invalid-ati-analyze.stdout 2> $(OFFLINE_LOG_DIR)/invalid-ati-analyze.stderr; rc=$$?; printf 'invalid_ati_analyze_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-ati-analyze.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-ati --probe-ati > $(OFFLINE_LOG_DIR)/invalid-ati-twice.stdout 2> $(OFFLINE_LOG_DIR)/invalid-ati-twice.stderr; rc=$$?; printf 'invalid_ati_twice_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-ati-twice.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --wait-reconnect --probe-ati > $(OFFLINE_LOG_DIR)/invalid-wait-ati.stdout 2> $(OFFLINE_LOG_DIR)/invalid-wait-ati.stderr; rc=$$?; printf 'invalid_wait_ati_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-wait-ati.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --help --probe-ati > $(OFFLINE_LOG_DIR)/invalid-help-ati.stdout 2> $(OFFLINE_LOG_DIR)/invalid-help-ati.stderr; rc=$$?; printf 'invalid_help_ati_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-help-ati.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --fixture --probe-ati > $(OFFLINE_LOG_DIR)/invalid-fixture-ati.stdout 2> $(OFFLINE_LOG_DIR)/invalid-fixture-ati.stderr; rc=$$?; printf 'invalid_fixture_ati_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-fixture-ati.status; test "$$rc" -eq 2
	grep -q -- '--probe-at ' $(OFFLINE_LOG_DIR)/help.stdout
	grep -q -- '--probe-ati' $(OFFLINE_LOG_DIR)/help.stdout
	@./build/dji-lte-mac --probe-status extra > $(OFFLINE_LOG_DIR)/invalid-status-extra.stdout 2> $(OFFLINE_LOG_DIR)/invalid-status-extra.stderr; rc=$$?; printf 'invalid_status_extra_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-status-extra.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-status --probe-at > $(OFFLINE_LOG_DIR)/invalid-status-at.stdout 2> $(OFFLINE_LOG_DIR)/invalid-status-at.stderr; rc=$$?; printf 'invalid_status_at_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-status-at.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-at --probe-status > $(OFFLINE_LOG_DIR)/invalid-at-status.stdout 2> $(OFFLINE_LOG_DIR)/invalid-at-status.stderr; rc=$$?; printf 'invalid_at_status_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-at-status.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-ati --probe-status > $(OFFLINE_LOG_DIR)/invalid-ati-status.stdout 2> $(OFFLINE_LOG_DIR)/invalid-ati-status.stderr; rc=$$?; printf 'invalid_ati_status_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-ati-status.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-status --probe-ati > $(OFFLINE_LOG_DIR)/invalid-status-ati.stdout 2> $(OFFLINE_LOG_DIR)/invalid-status-ati.stderr; rc=$$?; printf 'invalid_status_ati_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-status-ati.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --observe --probe-status > $(OFFLINE_LOG_DIR)/invalid-observe-status.stdout 2> $(OFFLINE_LOG_DIR)/invalid-observe-status.stderr; rc=$$?; printf 'invalid_observe_status_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-observe-status.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-status --observe > $(OFFLINE_LOG_DIR)/invalid-status-observe.stdout 2> $(OFFLINE_LOG_DIR)/invalid-status-observe.stderr; rc=$$?; printf 'invalid_status_observe_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-status-observe.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-status --probe-status > $(OFFLINE_LOG_DIR)/invalid-status-twice.stdout 2> $(OFFLINE_LOG_DIR)/invalid-status-twice.stderr; rc=$$?; printf 'invalid_status_twice_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-status-twice.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --fixture --probe-status > $(OFFLINE_LOG_DIR)/invalid-fixture-status.stdout 2> $(OFFLINE_LOG_DIR)/invalid-fixture-status.stderr; rc=$$?; printf 'invalid_fixture_status_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-fixture-status.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --help --probe-status > $(OFFLINE_LOG_DIR)/invalid-help-status.stdout 2> $(OFFLINE_LOG_DIR)/invalid-help-status.stderr; rc=$$?; printf 'invalid_help_status_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-help-status.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --wait-reconnect --probe-status > $(OFFLINE_LOG_DIR)/invalid-wait-status.stdout 2> $(OFFLINE_LOG_DIR)/invalid-wait-status.stderr; rc=$$?; printf 'invalid_wait_status_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-wait-status.status; test "$$rc" -eq 2
	grep -q -- '--probe-status' $(OFFLINE_LOG_DIR)/help.stdout
	@./build/dji-lte-mac --probe-pdp extra > $(OFFLINE_LOG_DIR)/invalid-pdp-extra.stdout 2> $(OFFLINE_LOG_DIR)/invalid-pdp-extra.stderr; rc=$$?; printf 'invalid_pdp_extra_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-pdp-extra.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-pdp --probe-status > $(OFFLINE_LOG_DIR)/invalid-pdp-status.stdout 2> $(OFFLINE_LOG_DIR)/invalid-pdp-status.stderr; rc=$$?; printf 'invalid_pdp_status_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-pdp-status.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-status --probe-pdp > $(OFFLINE_LOG_DIR)/invalid-status-pdp.stdout 2> $(OFFLINE_LOG_DIR)/invalid-status-pdp.stderr; rc=$$?; printf 'invalid_status_pdp_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-status-pdp.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-at --probe-pdp > $(OFFLINE_LOG_DIR)/invalid-at-pdp.stdout 2> $(OFFLINE_LOG_DIR)/invalid-at-pdp.stderr; rc=$$?; printf 'invalid_at_pdp_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-at-pdp.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-pdp --probe-ati > $(OFFLINE_LOG_DIR)/invalid-pdp-ati.stdout 2> $(OFFLINE_LOG_DIR)/invalid-pdp-ati.stderr; rc=$$?; printf 'invalid_pdp_ati_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-pdp-ati.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --observe --probe-pdp > $(OFFLINE_LOG_DIR)/invalid-observe-pdp.stdout 2> $(OFFLINE_LOG_DIR)/invalid-observe-pdp.stderr; rc=$$?; printf 'invalid_observe_pdp_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-observe-pdp.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-pdp --probe-pdp > $(OFFLINE_LOG_DIR)/invalid-pdp-twice.stdout 2> $(OFFLINE_LOG_DIR)/invalid-pdp-twice.stderr; rc=$$?; printf 'invalid_pdp_twice_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-pdp-twice.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --fixture --probe-pdp > $(OFFLINE_LOG_DIR)/invalid-fixture-pdp.stdout 2> $(OFFLINE_LOG_DIR)/invalid-fixture-pdp.stderr; rc=$$?; printf 'invalid_fixture_pdp_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-fixture-pdp.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --help --probe-pdp > $(OFFLINE_LOG_DIR)/invalid-help-pdp.stdout 2> $(OFFLINE_LOG_DIR)/invalid-help-pdp.stderr; rc=$$?; printf 'invalid_help_pdp_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-help-pdp.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --wait-reconnect --probe-pdp > $(OFFLINE_LOG_DIR)/invalid-wait-pdp.stdout 2> $(OFFLINE_LOG_DIR)/invalid-wait-pdp.stderr; rc=$$?; printf 'invalid_wait_pdp_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-wait-pdp.status; test "$$rc" -eq 2
	grep -q -- '--probe-pdp' $(OFFLINE_LOG_DIR)/help.stdout
	@./build/dji-lte-mac --probe-data-support extra > $(OFFLINE_LOG_DIR)/invalid-data-support-extra.stdout 2> $(OFFLINE_LOG_DIR)/invalid-data-support-extra.stderr; rc=$$?; printf 'invalid_data_support_extra_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-data-support-extra.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-data-support --probe-at > $(OFFLINE_LOG_DIR)/invalid-data-support-at.stdout 2> $(OFFLINE_LOG_DIR)/invalid-data-support-at.stderr; rc=$$?; printf 'invalid_data_support_at_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-data-support-at.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-at --probe-data-support > $(OFFLINE_LOG_DIR)/invalid-at-data-support.stdout 2> $(OFFLINE_LOG_DIR)/invalid-at-data-support.stderr; rc=$$?; printf 'invalid_at_data_support_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-at-data-support.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-data-support --probe-ati > $(OFFLINE_LOG_DIR)/invalid-data-support-ati.stdout 2> $(OFFLINE_LOG_DIR)/invalid-data-support-ati.stderr; rc=$$?; printf 'invalid_data_support_ati_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-data-support-ati.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-ati --probe-data-support > $(OFFLINE_LOG_DIR)/invalid-ati-data-support.stdout 2> $(OFFLINE_LOG_DIR)/invalid-ati-data-support.stderr; rc=$$?; printf 'invalid_ati_data_support_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-ati-data-support.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-data-support --probe-status > $(OFFLINE_LOG_DIR)/invalid-data-support-status.stdout 2> $(OFFLINE_LOG_DIR)/invalid-data-support-status.stderr; rc=$$?; printf 'invalid_data_support_status_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-data-support-status.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-status --probe-data-support > $(OFFLINE_LOG_DIR)/invalid-status-data-support.stdout 2> $(OFFLINE_LOG_DIR)/invalid-status-data-support.stderr; rc=$$?; printf 'invalid_status_data_support_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-status-data-support.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-data-support --probe-pdp > $(OFFLINE_LOG_DIR)/invalid-data-support-pdp.stdout 2> $(OFFLINE_LOG_DIR)/invalid-data-support-pdp.stderr; rc=$$?; printf 'invalid_data_support_pdp_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-data-support-pdp.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-pdp --probe-data-support > $(OFFLINE_LOG_DIR)/invalid-pdp-data-support.stdout 2> $(OFFLINE_LOG_DIR)/invalid-pdp-data-support.stderr; rc=$$?; printf 'invalid_pdp_data_support_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-pdp-data-support.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --observe --probe-data-support > $(OFFLINE_LOG_DIR)/invalid-observe-data-support.stdout 2> $(OFFLINE_LOG_DIR)/invalid-observe-data-support.stderr; rc=$$?; printf 'invalid_observe_data_support_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-observe-data-support.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-data-support --observe > $(OFFLINE_LOG_DIR)/invalid-data-support-observe.stdout 2> $(OFFLINE_LOG_DIR)/invalid-data-support-observe.stderr; rc=$$?; printf 'invalid_data_support_observe_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-data-support-observe.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-data-support --analyze > $(OFFLINE_LOG_DIR)/invalid-data-support-analyze.stdout 2> $(OFFLINE_LOG_DIR)/invalid-data-support-analyze.stderr; rc=$$?; printf 'invalid_data_support_analyze_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-data-support-analyze.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --probe-data-support --probe-data-support > $(OFFLINE_LOG_DIR)/invalid-data-support-twice.stdout 2> $(OFFLINE_LOG_DIR)/invalid-data-support-twice.stderr; rc=$$?; printf 'invalid_data_support_twice_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-data-support-twice.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --fixture --probe-data-support > $(OFFLINE_LOG_DIR)/invalid-fixture-data-support.stdout 2> $(OFFLINE_LOG_DIR)/invalid-fixture-data-support.stderr; rc=$$?; printf 'invalid_fixture_data_support_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-fixture-data-support.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --help --probe-data-support > $(OFFLINE_LOG_DIR)/invalid-help-data-support.stdout 2> $(OFFLINE_LOG_DIR)/invalid-help-data-support.stderr; rc=$$?; printf 'invalid_help_data_support_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-help-data-support.status; test "$$rc" -eq 2
	@./build/dji-lte-mac --wait-reconnect --probe-data-support > $(OFFLINE_LOG_DIR)/invalid-wait-data-support.stdout 2> $(OFFLINE_LOG_DIR)/invalid-wait-data-support.stderr; rc=$$?; printf 'invalid_wait_data_support_exit=%s\n' "$$rc" > $(OFFLINE_LOG_DIR)/invalid-wait-data-support.status; test "$$rc" -eq 2
	grep -q -- '--probe-data-support' $(OFFLINE_LOG_DIR)/help.stdout
	! grep -nE 'ClearPipeStall|ResetDevice|ResetPipe|SetConfiguration|SetAlternateInterface|ControlRequest|DeviceRequest|OpenSeize|WritePipeAsync|WriteIsochPipe|kIOServiceInteractionAllowed' src/*.c src/*.h
	@# Modem recovery: one plain device open and one re-enumeration, in usb_recover.c only; never a seizing open.
	test "$$(grep -l 'ReEnumerate' src/*.c src/*.h)" = "src/usb_recover.c"
	test "$$(grep -c 'ReEnumerate' src/usb_recover.c)" -eq 1
	test "$$(grep -c -- '->USBDeviceReEnumerate(dev, 0)' src/usb_recover.c)" -eq 1
	test "$$(grep -l 'USBDeviceOpen' src/*.c src/*.h)" = "src/usb_recover.c"
	test "$$(grep -c 'USBDeviceOpen' src/usb_recover.c)" -eq 1
	test "$$(grep -c -- '->USBDeviceOpen(dev)' src/usb_recover.c)" -eq 1
	test "$$(grep -l 'usb_recover_reenumerate(' src/*.c | tr '\n' ' ')" = "src/ppp_lte_up.c src/usb_recover.c "
	test "$$(grep -c 'usb_recover_reenumerate(device)' src/ppp_lte_up.c)" -eq 1
	! grep -rn 'OpenSeize' src
	test "$$(grep -oE '\(\*[a-z]+\)->[A-Za-z]+' src/usb_recover.c | LC_ALL=C sort -u | tr '\n' ' ')" = "(*dev)->Release (*dev)->USBDeviceClose (*dev)->USBDeviceOpen (*dev)->USBDeviceReEnumerate (*device)->QueryInterface "
	! grep -nE 'USBDeviceSuspend|AbortPipeZero|RequestExtraPower' src/*.c src/*.h
	test "$$(grep -lE 'kIOUSBDeviceInterfaceID[0-9]{3}' src/*.c src/*.h)" = "src/usb_recover.c"
	test "$$(grep -oE 'kIOUSBDeviceInterfaceID[0-9]+' src/usb_recover.c | LC_ALL=C sort -u)" = "kIOUSBDeviceInterfaceID187"
	! grep -nE 'ReadPipeAsync|AbortPipe|CreateInterfaceAsyncEventSource' $$(find src -type f \( -name '*.c' -o -name '*.h' \) ! -name ppp_usb.c)
	test "$$(grep -c -- '->ReadPipeAsyncTO(interface, in, slot->input, sizeof(slot->input),' src/ppp_usb.c)" -eq 1
	test "$$(grep -c -- '->AbortPipe(interface, a->in_pipe)' src/ppp_usb.c)" -eq 1
	test "$$(grep -c -- '->CreateInterfaceAsyncEventSource(interface, &a->source)' src/ppp_usb.c)" -eq 1
	test "$$(grep -Ec 'ReadPipeAsync|AbortPipe|CreateInterfaceAsyncEventSource' src/ppp_usb.c)" -eq 3
	test "$$(grep -o -- '->WritePipe' src/*.c src/*.h | wc -l | tr -d ' ')" -eq 1
	test "$$(grep -lwE 'defaultroute|usepeerdns' src/*.c src/*.h)" = "src/ppp_launch.c"
	test "$$(grep -c 'ppp_launch_prepare_up(&policy)' src/ppp_supervisor.c)" -eq 1
	test "$$(grep -l 'PPP_LAUNCH_MODE_UP' src/*.c src/*.h | tr '\n' ' ')" = "src/ppp_lte_up.c src/ppp_session.c src/ppp_session.h "
	test "$$(grep -o 'SCDynamicStoreRemoveValue(store, service_keys\[i\])' src/ppp_lte_service.c | wc -l | tr -d ' ')" -eq 1
	@./build/ppp_lte_upd extra > $(OFFLINE_LOG_DIR)/lte-upd-extra.stdout 2> $(OFFLINE_LOG_DIR)/lte-upd-extra.stderr; rc=$$?; test "$$rc" -eq 2
	! grep -nE 'SCDynamic|SCPreferences|WritePipe|ReadPipe|USBInterfaceOpen|USBDeviceOpen|system\(|popen|execv|execl' src/ppp_lte_upd.c
	test "$$(grep -c 'posix_spawn(pid, path' src/ppp_lte_upd.c)" -eq 1
	test "$$(grep -c 'u->b->spawn(u->b->context, UPD_CHILD_PATH, mode' src/ppp_lte_upd.c)" -eq 1
	./build/dji-dongle --help > $(OFFLINE_LOG_DIR)/dji-dongle-help.stdout
	grep -q -- 'reconnect' $(OFFLINE_LOG_DIR)/dji-dongle-help.stdout
	@./build/dji-dongle stop > $(OFFLINE_LOG_DIR)/dji-dongle-invalid.stdout 2> $(OFFLINE_LOG_DIR)/dji-dongle-invalid.stderr; rc=$$?; test "$$rc" -eq 2
	@./build/dji-dongle pause extra > $(OFFLINE_LOG_DIR)/dji-dongle-extra.stdout 2> $(OFFLINE_LOG_DIR)/dji-dongle-extra.stderr; rc=$$?; test "$$rc" -eq 2
	! grep -nE 'TODO|FIXME|HACK|test\.(skip|only)|\.skip\(|\.only\(|placeholder' src/ppp_lte_upd.c src/dji_dongle.c tests/ppp_lte_upd_test.c tests/dji_dongle_test.c
	test "$$(grep -l 'control\.sock' src/*.c src/*.h | tr '\n' ' ')" = "src/dji_dongle.c src/ppp_lte_upd.c "
	! grep -nE '\b(unlink|rename|chown|chmod|mkdir)\(' src/ppp_lte_upd.c | grep -v 'mkdir(path, 0700)'
	! grep -n 'dji-cellular-dongle-mac.sock' src/*.c src/*.h README.md README.ko.md docs/ppp-design.md
	! grep -nE 'inet_ntop|inet_ntoa|getnameinfo|ifa_netmask|ifa_dstaddr|system\(|popen|execv|execl|posix_spawn|setuid|IOKit|SCDynamic' src/dji_dongle.c
	./build/agent_app_test
	codesign --verify --strict 'build/DJI Dongle.app'
	test "$$(/usr/libexec/PlistBuddy -c 'Print :LSUIElement' 'build/DJI Dongle.app/Contents/Info.plist')" = true
	test "$$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' 'build/DJI Dongle.app/Contents/Info.plist')" = '$(APP_BUNDLE_ID)'
	test "$$(lipo -archs 'build/DJI Dongle.app/Contents/MacOS/DJIDongle')" = '$(APP_ARCH)'
	! grep -rnE 'Process\(|NSTask|NSAppleScript|OSAScript|URLSession|NWConnection|NWPathMonitor|CFSocket|getaddrinfo|inet_ntop|inet_ntoa|getnameinfo|ifa_netmask|ifa_dstaddr|system\(|popen|posix_spawn|execv|setuid|AuthorizationExecute|SCDynamic|SCPreferences|IOKit' agent/app/Sources agent/app/Tests
	! grep -rnE 'TODO|FIXME|HACK|placeholder' agent/app
	@for f in $(APP_STRINGS); do sed -n 's/^"\([^"]*\)" = .*/\1/p' "$$f" | sort > "$(OFFLINE_LOG_DIR)/$$(basename $$(dirname $$f)).keys"; done
	diff -u $(OFFLINE_LOG_DIR)/en.lproj.keys $(OFFLINE_LOG_DIR)/ko.lproj.keys
	@# Same format specifiers, in the same order, for every key.
	@for f in $(APP_STRINGS); do awk -F'" = "' '/^"/ { k = substr($$1, 2); v = $$2; o = ""; \
	  while (match(v, /%[0-9.]*(ll|l)?[@dsuf%]/)) { o = o substr(v, RSTART, RLENGTH) " "; v = substr(v, RSTART + RLENGTH) } \
	  print k "\t" o }' "$$f" | sort > "$(OFFLINE_LOG_DIR)/$$(basename $$(dirname $$f)).formats"; done
	diff -u $(OFFLINE_LOG_DIR)/en.lproj.formats $(OFFLINE_LOG_DIR)/ko.lproj.formats
	@grep -rhoE 'L\("[a-z.-]+"' agent/app/Sources | sed 's/^L("//; s/"$$//' | sort -u > $(OFFLINE_LOG_DIR)/app-used.keys
	comm -23 $(OFFLINE_LOG_DIR)/app-used.keys $(OFFLINE_LOG_DIR)/en.lproj.keys | { ! grep .; }
	test "$$(grep -c 'upd_commands\[\] = {"status", "pause", "resume", "reconnect"}' src/ppp_lte_upd.c)" -eq 1
	test "$$(grep -c 'SCDynamicStoreRemoveValue' src/ppp_lte_service.c)" -eq 1
	test "$$(grep -l 'SCDynamicStoreRemoveValue' src/*.c src/*.h)" = "src/ppp_lte_service.c"
	! grep -nE 'SCDynamicStoreSetValue|SCDynamicStoreSetMultiple|SCDynamicStoreAddValue|SCDynamicStoreAddTemporaryValue|SCDynamicStoreNotifyValue|SCPreferences|SCNetworkSet' src/*.c src/*.h
