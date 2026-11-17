# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corTestFunctions.sh - the verbs corCep's functional tests use; corTest sources it into every section
#
# The broker is a fixture, not the thing under test: ONE broker for the whole run (test/funcTests/broker.sh,
# `make functest` starts and stops it), any NGSI-LD broker. Each test isolates itself with a TENANT of its
# own, new on every run - nothing a test makes can be seen by another test, or by the same test's next run.
# corCep is started by each test, on a port of its own.
#
# corTest runs --INIT--, --RUN-- and --TEARDOWN-- as separate processes: what one section makes for the
# next (the tenant, corCep's pid) is kept in files under $CEP_TEST_DIR.
#
#   CEP_BROKER_HOST   the broker's host     (default 127.0.0.1)
#   CEP_BROKER_PORT   the broker's port     (default 4062 - clear of corTest's 1026-1035 and demo brokers on 4040)
#   CEP_PORT          corCep's notification port (default 7620)
#   CEP_BIN           the corCep binary     (default: corCep in this repo)
#   CEP_TEST_DIR      scratch: the tenant, corCep's pid and log (default /tmp/corcep-functest)
#
CEP_BROKER_HOST="${CEP_BROKER_HOST:-127.0.0.1}"
CEP_BROKER_PORT="${CEP_BROKER_PORT:-4062}"
CEP_PORT="${CEP_PORT:-7620}"
CEP_BIN="${CEP_BIN:-${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}/corCep}"
CEP_TEST_DIR="${CEP_TEST_DIR:-/tmp/corcep-functest}"
CEP_AUTH_DIR="${CEP_AUTH_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/hub}"   # broker.sh up: the auth broker's material


# -----------------------------------------------------------------------------
#
# cepTestInit - the test's scratch directory, emptied, and a new tenant for it (--INIT--)
#
cepTestInit() {
  cepStop
  rm -rf "${CEP_TEST_DIR:?}"
  mkdir -p "$CEP_TEST_DIR"
  echo "cep$(date +%s%N | tail -c 10)$$" > "$CEP_TEST_DIR/tenant"
}


# -----------------------------------------------------------------------------
#
# cepTenant - the test's tenant
#
cepTenant() {
  cat "$CEP_TEST_DIR/tenant"
}


# -----------------------------------------------------------------------------
#
# ngsild <method> <path> [payload] - an NGSI-LD request to the broker, in the test's tenant
#
# Prints what corCurl prints: the status line, the headers, the body (JSON members sorted).
#
ngsild() {
  local method="$1" path="$2" payload="$3"
  local -a args=(--host "$CEP_BROKER_HOST" --port "$CEP_BROKER_PORT" -X "$method" --url "/ngsi-ld/v1$path" -H "NGSILD-Tenant: $(cepTenant)")

  [ -z "$payload" ] || args+=(--payload "$payload")
  corCurl "${args[@]}"
}


# -----------------------------------------------------------------------------
#
# cepStart [options...] - corCep against the broker, in the test's tenant; returns once its first rules are read
#
# The log (stderr) goes to $CEP_TEST_DIR/corCep.log - a test waits on it with cepAwaitLog, and it is kept for
# reading when a test fails.
#
# CEP_BROKER_URL: another broker URL for corCep (a test's proxy - ftFlakyBroker.py); default the test broker
cepStart() {
  rm -f "$CEP_TEST_DIR/corCep.log"               # not the last corCep's: its "rules" line is not this one's
  "$CEP_BIN" --broker "${CEP_BROKER_URL:-http://$CEP_BROKER_HOST:$CEP_BROKER_PORT}" --port "$CEP_PORT" --tenant "$(cepTenant)" "$@" \
             > "$CEP_TEST_DIR/corCep.log" 2>&1 &
  echo $! > "$CEP_TEST_DIR/corCep.pid"

  cepAwaitLog " rules\?, [0-9]* subscriptions\?$" 10 > /dev/null
}


# -----------------------------------------------------------------------------
#
# cepStop - stop the corCep cepStart started (by its pid, never by name)
#
# cepPidStop <pidFile> - a SIGTERM, then waits for the exit (corCep writes its state and statistics first - a second
# at most), kill -9 after 5 s; the pid file removed. Its port is free when this returns.
cepPidStop() {
  local pidFile="$1"
  [ -f "$pidFile" ] || return 0

  local pid i
  pid=$(cat "$pidFile")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  rm -f "$pidFile"
}
cepStop() {
  cepPidStop "$CEP_TEST_DIR/corCep.pid"
}


# -----------------------------------------------------------------------------
#
# cepAwaitLog <regex> [seconds] - wait until a line of corCep's log matches (grep BRE), then print the line
# without its timestamp
#
cepAwaitLog() {
  local secs="${2:-5}" i line

  for ((i = 0; i < secs * 10; i++)); do
    line=$(grep -- "$1" "$CEP_TEST_DIR/corCep.log" 2>/dev/null | tail -1)
    if [ -n "$line" ]; then
      echo "${line#* * }"
      return 0
    fi
    if ! kill -0 "$(cat "$CEP_TEST_DIR/corCep.pid" 2>/dev/null)" 2>/dev/null; then
      echo "cepAwaitLog: corCep is not running - $CEP_TEST_DIR/corCep.log:" >&2
      tail -20 "$CEP_TEST_DIR/corCep.log" >&2
      return 1
    fi
    sleep 0.1
  done

  echo "cepAwaitLog: no line matching '$1' after ${secs}s - $CEP_TEST_DIR/corCep.log:" >&2
  tail -20 "$CEP_TEST_DIR/corCep.log" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# attrAwait <entityId> <attr> <value> [seconds] - wait until the attribute's value (keyValues) is <value>,
# given as JSON (true, 42, "on")
#
attrAwait() {
  local url="http://$CEP_BROKER_HOST:$CEP_BROKER_PORT/ngsi-ld/v1/entities/$1?options=keyValues&attrs=$2"
  local secs="${4:-5}" i body

  for ((i = 0; i < secs * 10; i++)); do
    body=$(curl -s -H "NGSILD-Tenant: $(cepTenant)" "$url")
    echo "$body" | grep -qE "\"$2\": *$3[,}]" && return 0
    sleep 0.1
  done

  echo "attrAwait: $1 $2 is not $3 after ${secs}s: $body" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# ngsildStatus <method> <path> [payload] - an NGSI-LD request in the test's tenant; prints '<method> <path>: <status>'
#
# For the setup steps of a test, where only the status matters.
#
ngsildStatus() {
  local method="$1" path="$2" payload="$3"
  local -a args=(-s -o /dev/null -w '%{http_code}' -X "$method" -H "NGSILD-Tenant: $(cepTenant)")

  [ -z "$payload" ] || args+=(-H "Content-Type: application/json" -d "$payload")
  echo "$method $path: $(curl "${args[@]}" "http://$CEP_BROKER_HOST:$CEP_BROKER_PORT/ngsi-ld/v1$path")"
}


# -----------------------------------------------------------------------------
#
# entityKv <entityId> - the entity, keyValues, on one line, members sorted
#
entityKv() {
  curl -s -H "NGSILD-Tenant: $(cepTenant)" "http://$CEP_BROKER_HOST:$CEP_BROKER_PORT/ngsi-ld/v1/entities/$1?options=keyValues" |
    python3 -I -c 'import json, sys; print(json.dumps(json.load(sys.stdin), sort_keys=True))'
}


# -----------------------------------------------------------------------------
#
# cepSubs - corCep's subscriptions in the test's tenant, their ids, sorted
#
cepSubs() {
  curl -s -H "NGSILD-Tenant: $(cepTenant)" "http://$CEP_BROKER_HOST:$CEP_BROKER_PORT/ngsi-ld/v1/subscriptions?limit=1000" |
    grep -o '"urn:ngsi-ld:Subscription:corcep-[^"]*"' | tr -d '"' | sort
}


# -----------------------------------------------------------------------------
#
# subsAwait <n> [seconds] - wait until corCep has <n> event subscriptions (corcep-<number>; corcep-rules not counted)
#
subsAwait() {
  local secs="${2:-5}" i n

  for ((i = 0; i < secs * 10; i++)); do
    n=$(cepSubs | grep -c 'corcep-[0-9]*$')
    [ "$n" == "$1" ] && return 0
    sleep 0.1
  done

  echo "subsAwait: $n event subscriptions, not $1, after ${secs}s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# firedCount <rule> - the times corCep's log says the rule fired (the rule's id after urn:ngsi-ld:Rule:)
#
firedCount() {
  grep -c "rule urn:ngsi-ld:Rule:$1 fired" "$CEP_TEST_DIR/corCep.log"
}


# -----------------------------------------------------------------------------
#
# firedAwait <rule> <n> [seconds] - wait until the rule has fired <n> times; then print '<rule>: <n>'
#
# A 'sentinel' rule - a single-event rule on the same event as the rule under test - makes a negative check
# exact: corCep feeds every rule of an event before the next event, the patterns first, so once the sentinel
# has fired n times, the rule under test has seen those n events.
#
firedAwait() {
  local secs="${3:-5}" i n

  for ((i = 0; i < secs * 20; i++)); do
    n=$(firedCount "$1")
    [ "$n" -ge "$2" ] && { echo "$1: $n"; return 0; }
    sleep 0.05
  done

  echo "firedAwait: $1 fired $n times, not $2, after ${secs}s" >&2
  echo "$1: $n"
  return 1
}


# -----------------------------------------------------------------------------
#
# msNow - the time in milliseconds, from bash itself ($EPOCHREALTIME: seconds, the locale's radix, 6 digits)
#
# Not `date +%s%3N`: uutils' date (the CI image's) ignores the 3 and prints nanoseconds.
#
msNow() {
  local us="${EPOCHREALTIME//[.,]/}"
  echo $((us / 1000))
}


# -----------------------------------------------------------------------------
#
# press <entityId> <attribute> - an update of the attribute (the value 1 each time - the same value is an event too)
#
press() {
  curl -s -o /dev/null -X PATCH -H "NGSILD-Tenant: $(cepTenant)" -H "Content-Type: application/json" \
       -d "{\"$2\": {\"type\": \"Property\", \"value\": 1}}" \
       "http://$CEP_BROKER_HOST:$CEP_BROKER_PORT/ngsi-ld/v1/entities/$1/attrs"
}


# -----------------------------------------------------------------------------
#
# attrSet <entityId> <attribute> <value> - an update of the attribute to <value>, given as JSON (21.5, "n/a")
#
attrSet() {
  curl -s -o /dev/null -X PATCH -H "NGSILD-Tenant: $(cepTenant)" -H "Content-Type: application/json" \
       -d "{\"$2\": {\"type\": \"Property\", \"value\": $3}}" \
       "http://$CEP_BROKER_HOST:$CEP_BROKER_PORT/ngsi-ld/v1/entities/$1/attrs"
}


# -----------------------------------------------------------------------------
#
# /dry-run - the rule editors' tokens and the replay (src/dryrun.c)
#
#   dryRunKeys                  an ES256 key (k1) and its JWK Set in $CEP_TEST_DIR (--jwks $CEP_TEST_DIR/jwks.json)
#   dryRunToken [ftJwt sign options...]   a token signed with k1, iss http://editor.test, for 300 s
#   dryRunPost <token> <body>   POST /dry-run: the status, then the body - JSON, members sorted, indented
#
dryRunKeys() {
  python3 -I "$(dirname "${BASH_SOURCE[0]}")/tools/ftJwt.py" key --out "$CEP_TEST_DIR" --name k1 --alg ES256
  python3 -I "$(dirname "${BASH_SOURCE[0]}")/tools/ftJwt.py" jwks --out "$CEP_TEST_DIR/jwks.json" "$CEP_TEST_DIR/k1.jwk"
}

dryRunToken() {
  python3 -I "$(dirname "${BASH_SOURCE[0]}")/tools/ftJwt.py" sign --key "$CEP_TEST_DIR/k1.pem" --alg ES256 \
          --iss http://editor.test --exp 300 "$@"
}

dryRunPost() {
  local token="$1" body="$2" out
  local -a auth=()

  [ -z "$token" ] || auth=(-H "Authorization: Bearer $token")
  out=$(curl -s -w '\n%{http_code}' -X POST "http://127.0.0.1:$CEP_PORT/dry-run" "${auth[@]}" \
             -H 'Content-Type: application/json' --data-binary "$body")
  echo "POST /dry-run: ${out##*$'\n'}"
  printf '%s' "${out%$'\n'*}" | python3 -I -c 'import json, sys; print(json.dumps(json.load(sys.stdin), indent=2, sort_keys=True))'
}


# -----------------------------------------------------------------------------
#
# flakyBrokerStart <port> <fail file> <log file> - ftFlakyBroker.py in front of the test broker (pid in
# $CEP_TEST_DIR/proxy.pid - cepPidStop it); waits till it answers
#
flakyBrokerStart() {
  python3 -I "$(dirname "${BASH_SOURCE[0]}")/tools/ftFlakyBroker.py" "$1" "http://$CEP_BROKER_HOST:$CEP_BROKER_PORT" "$2" "$3" &
  echo $! > "$CEP_TEST_DIR/proxy.pid"

  local i
  for ((i = 0; i < 50; i++)); do
    curl -s -o /dev/null "http://127.0.0.1:$1/" && return 0
    sleep 0.1
  done
  echo "flakyBrokerStart: no answer on port $1" >&2
  return 1
}

#
# pdpStart <port> <policy file> <log file> - the stand-in policy decision point (tools/ftPdp.py), its pid in
# $CEP_TEST_DIR/pdp.pid (cepPidStop it); waits till it answers
#
pdpStart() {
  python3 -I "$(dirname "${BASH_SOURCE[0]}")/tools/ftPdp.py" "$1" "$2" "$3" &
  echo $! > "$CEP_TEST_DIR/pdp.pid"

  local i
  for ((i = 0; i < 50; i++)); do
    curl -s -o /dev/null "http://127.0.0.1:$1/" && return 0
    sleep 0.1
  done
  echo "pdpStart: no answer on port $1" >&2
  return 1
}
