#!/bin/bash
#
# FILE            broker.sh
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# The broker the functional tests run against - a coraine container, on the host's network:
#
#   broker.sh up      start it and wait until it answers
#   broker.sh down    stop it
#
# The broker is a fixture: the tests isolate themselves by tenant, so one broker serves the whole run, and
# ramDB is enough - nothing outlives the container. Any NGSI-LD broker would do: skip `up`, set
# CEP_BROKER_HOST / CEP_BROKER_PORT, run corTest.
#
#   CEP_BROKER_IMAGE  default quay.io/seamware/coraine:0.7.0 - a released version, never :latest
#   CEP_BROKER_PORT   default 4062
#
# A second broker for cep_broker_auth.test, as a home hub runs one: HTTPS with a certificate for hub.example
# (reached at 127.0.0.1), short-lived tokens only (OIDC, ES256, a JWK Set file). Its material - the
# certificates, corCep's signing key, the JWK Set, token.py (prints a token) - made here, in CEP_AUTH_DIR
# (test/funcTests/hub - the same path relative to the repo on a workstation and in CI's build container).
#
#   CEP_AUTH_IMAGE    default quay.io/seamware/coraine:0.7.0-post-2026-11-12-9032b724 - main's build of 11-12:
#                     0.7.0 has no --https; a release once one has it
#   CEP_AUTH_PORT     default 4063
#
# Its subscriptions' owners hold 20 s (--oidcOwnerMaxAge; a hub: 24 h) - cep_renew_subscriptions.test sees one run out.
#
IMAGE="${CEP_BROKER_IMAGE:-quay.io/seamware/coraine:0.7.0}"
PORT="${CEP_BROKER_PORT:-4062}"
NAME="corcep-functest-broker-$PORT"
AUTH_IMAGE="${CEP_AUTH_IMAGE:-quay.io/seamware/coraine:0.7.0-post-2026-11-12-9032b724}"
AUTH_PORT="${CEP_AUTH_PORT:-4063}"
AUTH_NAME="corcep-functest-auth-$AUTH_PORT"
AUTH_DIR="$(cd "$(dirname "$0")" && pwd)/hub"

authMaterial() {
  rm -rf "$AUTH_DIR"
  mkdir -p "$AUTH_DIR"
  cd "$AUTH_DIR" || return 1
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout tls-key.pem -out cert.pem -days 2 \
          -subj "/CN=hub.example" -addext "subjectAltName=DNS:hub.example" 2> /dev/null || return 1
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout other-key.pem -out other.pem -days 2 \
          -subj "/CN=hub.example" -addext "subjectAltName=DNS:hub.example" 2> /dev/null || return 1
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out sign.pem 2> /dev/null || return 1
  cat > token.py <<'EOF'
# token.py <dir> <exp seconds> - an ES256 JWT for the test broker, signed with <dir>/sign.pem; --jwks: the JWK Set
import base64, json, os, subprocess, sys, time
d = sys.argv[1]
b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
der = subprocess.run(["openssl", "pkey", "-in", f"{d}/sign.pem", "-pubout", "-outform", "DER"], capture_output=True, check=True).stdout
x, y = der[-64:-32], der[-32:]
jwk = {"kty": "EC", "crv": "P-256", "x": b64(x), "y": b64(y), "kid": "cep-test", "alg": "ES256", "use": "sig"}
if sys.argv[2] == "--jwks":
    print(json.dumps({"keys": [jwk]}))
    sys.exit(0)
n = int(open(f"{d}/runs").read()) + 1 if os.path.exists(f"{d}/runs") else 1
if not os.environ.get("NOCOUNT"):                  # the test's own requests are not counted
    open(f"{d}/runs", "w").write(str(n))
if n == 1 and os.environ.get("BAD_FIRST"):
    print("not-a-token")
    sys.exit(0)
now = int(time.time())
head = b64(json.dumps({"alg": "ES256", "typ": "JWT", "kid": "cep-test"}).encode())
body = b64(json.dumps({"iss": "cep-test", "sub": "corcep", "iat": now, "exp": now + int(sys.argv[2]),
                       "realm_access": {"roles": ["ngsild-read", "ngsild-write"]}}).encode())
sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", f"{d}/sign.pem"], input=f"{head}.{body}".encode(),
                     capture_output=True, check=True).stdout
# DER ECDSA-Sig-Value -> r || s, 32 bytes each
i = 2 if sig[1] < 0x80 else 3
rl = sig[i + 1]; r = sig[i + 2:i + 2 + rl]; j = i + 2 + rl; sl = sig[j + 1]; s = sig[j + 2:j + 2 + sl]
fix = lambda v: v[-32:].rjust(32, b"\0")
print(f"{head}.{body}.{b64(fix(r) + fix(s))}")
EOF
  python3 -I token.py "$AUTH_DIR" --jwks > jwks.json || return 1
  chmod 755 "$AUTH_DIR"
  chmod 644 *.pem jwks.json token.py
  cd - > /dev/null
}

authUp() {
  authMaterial || { echo "broker.sh: the auth broker's material (openssl, python3) - not made in $AUTH_DIR" >&2; return 1; }
  docker rm -f "$AUTH_NAME" > /dev/null 2>&1
  docker run -d --rm --name "$AUTH_NAME" --network host -v "$AUTH_DIR:/hub:ro" "$AUTH_IMAGE" \
         --database ramDB --troe none --port "$AUTH_PORT" --https --key /hub/tls-key.pem --cert /hub/cert.pem \
         --auth oidc --oidcIssuer cep-test --oidcJwks /hub/jwks.json --oidcAnyTenant --oidcOwnerMaxAge 20 > /dev/null || return 1
  for ((i = 0; i < 150; i++)); do
    [ "$(curl -sk -o /dev/null -w '%{http_code}' "https://127.0.0.1:$AUTH_PORT/ngsi-ld/v1/types")" = 401 ] && { echo "broker: $AUTH_IMAGE on port $AUTH_PORT (https, oidc - $AUTH_DIR)"; return 0; }
    sleep 0.1
  done
  echo "broker.sh: $AUTH_IMAGE (https, oidc) does not answer on port $AUTH_PORT after 15s:" >&2
  docker logs "$AUTH_NAME" 2>&1 | tail -20 >&2
  return 1
}

case "$1" in
  up)
    docker rm -f "$NAME" > /dev/null 2>&1
    docker run -d --rm --name "$NAME" --network host "$IMAGE" --database ramDB --troe none --port "$PORT" --apiPlugins admin > /dev/null || exit 1

    for ((i = 0; i < 100; i++)); do
      curl -s -o /dev/null "http://127.0.0.1:$PORT/ngsi-ld/v1/types" && { echo "broker: $IMAGE on port $PORT"; authUp; exit $?; }
      sleep 0.1
    done

    echo "broker.sh: $IMAGE does not answer on port $PORT after 10s:" >&2
    docker logs "$NAME" 2>&1 | tail -20 >&2
    exit 1
    ;;

  down)
    docker rm -f "$NAME" "$AUTH_NAME" > /dev/null 2>&1
    rm -rf "$AUTH_DIR"
    exit 0
    ;;

  *)
    echo "usage: broker.sh up|down" >&2
    exit 1
    ;;
esac
