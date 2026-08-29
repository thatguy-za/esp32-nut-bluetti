# Host-side tests for the portable logic (no ESP-IDF needed).
#   make            build + run the C tests
#   make xcheck     build the crypto cross-check helper (run xcheck.py separately)
CC      ?= cc
CFLAGS  ?= -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter
EF      := ../components/ecoflow_ble
NUT     := ../components/nut_server
UECC    := ../components/micro_ecc/micro-ecc
INC     := -I$(EF)/private -I$(EF)/include -Ishim
UECC_DEFS := -DuECC_SUPPORTS_secp160r1=1 -DuECC_SUPPORTS_secp192r1=0 \
             -DuECC_SUPPORTS_secp224r1=0 -DuECC_SUPPORTS_secp256r1=0 \
             -DuECC_SUPPORTS_secp256k1=0 -DuECC_SUPPORT_COMPRESSED_POINT=0

.PHONY: all clean xcheck
all: ef_logic_test ef_river3_test nut_server_test auth_test
	./ef_logic_test
	./ef_river3_test
	./nut_server_test
	./auth_test

ef_logic_test: ef_logic_test.c ef_stub.c $(EF)/ef_frame.c $(EF)/ef_proto.c
	$(CC) $(CFLAGS) $(INC) $^ -lm -o $@

ef_river3_test: ef_river3_test.c ef_stub.c $(EF)/ef_frame.c $(EF)/ef_proto.c
	$(CC) $(CFLAGS) $(INC) $^ -lm -o $@

nut_server_test: nut_server_test.c $(NUT)/nut_server.c
	$(CC) $(CFLAGS) -Ishim -I$(NUT)/include $^ -lpthread -o $@

auth_test: auth_test.c
	$(CC) $(CFLAGS) $^ -o $@

xcheck: xcheck_helper.c $(UECC)/uECC.c
	$(CC) $(CFLAGS) -w -I$(UECC) $(UECC_DEFS) $^ -o $@

clean:
	rm -rf ef_logic_test ef_river3_test nut_server_test auth_test xcheck *.dSYM
