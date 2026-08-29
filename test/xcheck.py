"""Cross-check the C crypto (micro-ecc / md5 / session-key) against the
python-ecdsa reference the real EcoFlow device interoperates with.

Needs:  pip install ecdsa   and   `make xcheck` (builds ./xcheck)
"""
import subprocess, hashlib, os, sys, struct
import ecdsa
from ecdsa import SECP160r1

HERE = os.path.dirname(os.path.abspath(__file__))
XCHECK = os.path.join(HERE, "xcheck")
KEYDATA = open(os.path.join(HERE, "..", "components", "ecoflow_ble", "ef_keydata.bin"), "rb").read()

fails = 0
def check(ok, msg):
    global fails
    print(("ok:   " if ok else "FAIL: ") + msg)
    fails += 0 if ok else 1

def c(*a):
    return subprocess.run([XCHECK, *a], capture_output=True, text=True).stdout.strip()

# ---- ECDH secp160r1 interop (micro-ecc vs python-ecdsa) --------------
for _ in range(5):
    dev = ecdsa.SigningKey.generate(curve=SECP160r1)
    esp = ecdsa.SigningKey.generate(curve=SECP160r1)
    esp_priv = esp.to_string().hex()          # 21 bytes
    esp_pub = esp.get_verifying_key().to_string().hex()   # 40 bytes
    dev_pub = dev.get_verifying_key().to_string().hex()
    check(c("pub", esp_priv) == esp_pub, "micro-ecc pubkey == python-ecdsa")
    shared_ref = ecdsa.ECDH(SECP160r1, dev,
        ecdsa.VerifyingKey.from_string(bytes.fromhex(esp_pub), curve=SECP160r1)
        ).generate_sharedsecret_bytes().hex()
    shared_c = c("ecdh", esp_priv, dev_pub)
    check(shared_c == shared_ref, "micro-ecc ECDH shared secret == python-ecdsa")
    check(c("md5", shared_c) == hashlib.md5(bytes.fromhex(shared_c)).hexdigest(),
          "iv = md5(shared)")

# ---- session-key derivation (connection.py::_gen_session_key) --------
# seeds kept within the key table (pos + 16 <= len(KEYDATA))
for seed0, seed1 in [(0x2a, 0x11), (0x00, 0x01), (0xf0, 0xf0), (0x7c, 0x40)]:
    seed = bytes([seed0, seed1]); srand = bytes((i * 7 + 3) & 0xFF for i in range(16))
    pos = seed0 * 0x10 + ((seed1 - 1) & 0xFF) * 0x100
    assert pos + 16 <= len(KEYDATA), (seed.hex(), pos)
    kd16 = KEYDATA[pos:pos + 16]
    dn = [struct.unpack("<Q", kd16[0:8])[0], struct.unpack("<Q", kd16[8:16])[0],
          struct.unpack("<Q", srand[0:8])[0], struct.unpack("<Q", srand[8:16])[0]]
    ref = hashlib.md5(b"".join(struct.pack("<Q", x) for x in dn)).hexdigest()
    check(c("sesskey", kd16.hex(), srand.hex()) == ref, f"gen_session_key seed={seed.hex()}")

# ---- auto-auth token md5(user_id+sn).upper() -----------------------
uid, sn = "1234567890abcdef1234", "R655ZTEST0001XYZ"
check(c("md5", (uid + sn).encode().hex()).upper()
      == hashlib.md5((uid + sn).encode("ASCII")).hexdigest().upper(), "auth token")

print(f"\n{'FAILURES' if fails else 'ALL PASS'} ({fails} failures)")
sys.exit(1 if fails else 0)
