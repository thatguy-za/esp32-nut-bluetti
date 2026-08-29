# Host-side tests for the portable logic (no ESP-IDF needed).
#   make    build + run every suite
#
# The BLUETTI BLE layer has no tests yet: its protocol is unconfirmed, so
# there is nothing to assert against. Everything here covers the parts
# that are vendor-independent.
CC      ?= cc
CFLAGS  ?= -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter
NUT     := ../components/nut_server

.PHONY: all clean
all: nut_server_test auth_test ipv4_test lb_test
	./nut_server_test
	./auth_test
	./ipv4_test
	./lb_test

nut_server_test: nut_server_test.c $(NUT)/nut_server.c
	$(CC) $(CFLAGS) -Ishim -I$(NUT)/include $^ -lpthread -o $@

auth_test: auth_test.c
	$(CC) $(CFLAGS) $^ -o $@

ipv4_test: ipv4_test.c
	$(CC) $(CFLAGS) $^ -o $@

lb_test: lb_test.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -rf nut_server_test auth_test ipv4_test lb_test *.dSYM
