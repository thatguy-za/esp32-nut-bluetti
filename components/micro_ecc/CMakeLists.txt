# Vendored micro-ecc (Kenneth MacKay, BSD-2-Clause) — copied from the ESP-IDF
# bootloader subproject. We vendor it because IDF does not expose micro-ecc as a
# normal component, and we need the secp160r1 curve for the EcoFlow BLE handshake.
idf_component_register(
    SRCS "micro-ecc/uECC.c"
    INCLUDE_DIRS "micro-ecc"
)

# Only secp160r1 is used; drop the rest to save flash. RNG is provided by the
# platform hook we register at runtime (esp_fill_random via uECC_set_rng).
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    uECC_SUPPORTS_secp160r1=1
    uECC_SUPPORTS_secp192r1=0
    uECC_SUPPORTS_secp224r1=0
    uECC_SUPPORTS_secp256r1=0
    uECC_SUPPORTS_secp256k1=0
    uECC_SUPPORT_COMPRESSED_POINT=0
    uECC_OPTIMIZATION_LEVEL=3
)
