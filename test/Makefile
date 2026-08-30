# Host-side tests for the portable logic (no ESP-IDF needed).
#   make    build + run every suite
#
# bt_frame_test and bt_regs_test mirror the BLUETTI wire format and the
# Elite 10 register decode. They cannot prove the protocol is right — that
# needs hardware — but they pin the arithmetic.
CC      ?= cc
CFLAGS  ?= -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter
NUT     := ../components/nut_server

.PHONY: all clean
all: nut_server_test auth_test ipv4_test lb_test bt_frame_test bt_regs_test bt_device_test ota_gh_test
	./nut_server_test
	./auth_test
	./ipv4_test
	./lb_test
	./bt_frame_test
	./bt_regs_test
	./bt_device_test
	./ota_gh_test ota_gh_test

nut_server_test: nut_server_test.c $(NUT)/nut_server.c
	$(CC) $(CFLAGS) -Ishim -I$(NUT)/include $^ -lpthread -o $@

auth_test: auth_test.c
	$(CC) $(CFLAGS) $^ -o $@

ipv4_test: ipv4_test.c
	$(CC) $(CFLAGS) $^ -o $@

lb_test: lb_test.c
	$(CC) $(CFLAGS) $^ -o $@

bt_frame_test: bt_frame_test.c
	$(CC) $(CFLAGS) $^ -o $@

bt_regs_test: bt_regs_test.c
	$(CC) $(CFLAGS) $^ -o $@

bt_device_test: bt_device_test.c
	$(CC) $(CFLAGS) $^ -o $@

ota_gh_test: ota_gh_test.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -rf nut_server_test auth_test ipv4_test lb_test bt_frame_test bt_regs_test bt_device_test ota_gh_test *.dSYM
