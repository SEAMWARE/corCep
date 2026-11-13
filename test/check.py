#!/usr/bin/env python3
#
# FILE            check.py
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corCep's tests against a running NGSI-LD broker:  check.py <corCep binary> <broker URL>
#
# corCep is started here, on a port of its own; the entities and rules have ids of this run and are deleted at
# the end. Each check: what was done, what the broker holds after it.
#
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

CEP, BROKER = sys.argv[1], sys.argv[2].rstrip("/")
PORT        = int(os.environ.get("CORCEP_TEST_PORT", "7612"))
RUN         = f"{os.getpid()}"
API         = f"{BROKER}/ngsi-ld/v1"

fails  = 0
checks = 0


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req  = urllib.request.Request(f"{API}{path}", data=data, method=method)
    req.add_header("Accept", "application/json")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            text = r.read().decode()
            return r.status, (json.loads(text) if text else None)
    except urllib.error.HTTPError as e:
        text = e.read().decode()
        return e.code, (json.loads(text) if text else None)


def check(what, got, want):
    global fails, checks
    checks += 1
    ok = (got == want)
    if not ok:
        fails += 1
    print(f"{'ok  ' if ok else 'FAIL'} {what}" + ("" if ok else f" - got {got!r}, want {want!r}"))


def waitFor(what, fn, want, seconds=3.0):
    end = time.time() + seconds
    got = fn()
    while got != want and time.time() < end:
        time.sleep(0.05)
        got = fn()
    check(what, got, want)


def prop(v):
    return {"type": "Property", "value": v}


def attr(entity, name):
    s, b = call("GET", f"/entities/{entity}?options=keyValues")
    return b.get(name) if s == 200 else None


def rule(id, when, then, **extra):
    r = {"id": id, "type": "Rule", "when": {"type": "JsonProperty", "json": when}, "then": {"type": "JsonProperty", "json": then}}
    for k, v in extra.items():
        r[k] = prop(v)
    return r


def ourSubs():
    s, b = call("GET", "/subscriptions?limit=1000")
    return sorted(x["id"] for x in (b or []) if x["id"].startswith("urn:ngsi-ld:Subscription:corcep-"))


REMOTE = f"urn:ngsi-ld:Remote:t{RUN}"
LIGHT  = f"urn:ngsi-ld:Light:t{RUN}"
R_ON   = f"urn:ngsi-ld:Rule:t{RUN}-on"
R_TOG  = f"urn:ngsi-ld:Rule:t{RUN}-toggle"
R_BAD  = f"urn:ngsi-ld:Rule:t{RUN}-bad"
R_OFF  = f"urn:ngsi-ld:Rule:t{RUN}-disabled"

made = [REMOTE, LIGHT, R_ON, R_TOG, R_BAD, R_OFF]
log  = open(f"/tmp/corcep-test-{RUN}.log", "w")
cep  = subprocess.Popen([CEP, "--broker", BROKER, "--port", str(PORT)], stdout=log, stderr=subprocess.STDOUT)

try:
    time.sleep(0.8)
    check("corCep is running", cep.poll(), None)
    check("the Rule subscription", f"urn:ngsi-ld:Subscription:corcep-rules" in ourSubs(), True)

    call("POST", "/entities", {"id": REMOTE, "type": "Remote", "upperPress": prop(0), "lowerPress": prop(0)})
    call("POST", "/entities", {"id": LIGHT, "type": "Light", "on": prop(False), "level": prop(10)})

    # A rule made while corCep runs applies without a restart
    s, _ = call("POST", "/entities", rule(R_ON, {"entity": REMOTE, "attribute": "upperPress"},
                                          [{"entity": LIGHT, "set": {"on": True, "level": 200}}], name="upper: on"))
    check("POST the 'set' rule", s, 201)
    waitFor("its subscription made", lambda: "urn:ngsi-ld:Subscription:corcep-0" in ourSubs(), True)

    call("PATCH", f"/entities/{REMOTE}/attrs", {"upperPress": prop(1)})
    waitFor("set: the light on", lambda: attr(LIGHT, "on"), True)
    check("set: the level too", attr(LIGHT, "level"), 200)

    # toggle - and the SAME value again is an event too (a button pressed twice)
    call("POST", "/entities", rule(R_TOG, {"entity": REMOTE, "attribute": "lowerPress"}, [{"entity": LIGHT, "toggle": "on"}]))
    waitFor("the toggle rule's subscription", lambda: len([x for x in ourSubs() if x[-1].isdigit()]), 2)
    call("PATCH", f"/entities/{REMOTE}/attrs", {"lowerPress": prop(1)})
    waitFor("toggle: off", lambda: attr(LIGHT, "on"), False)
    call("PATCH", f"/entities/{REMOTE}/attrs", {"lowerPress": prop(1)})
    waitFor("toggle again, the same value: on", lambda: attr(LIGHT, "on"), True)

    # A rule that is not right is left out, the others run
    call("POST", "/entities", rule(R_BAD, {"attribute": "upperPress"}, [{"entity": LIGHT, "toggle": "on"}]))
    call("PATCH", f"/entities/{LIGHT}/attrs", {"level": prop(5)})
    call("PATCH", f"/entities/{REMOTE}/attrs", {"upperPress": prop(2)})
    waitFor("a bad rule beside them: the good one still fires", lambda: attr(LIGHT, "level"), 200)

    # enabled: false - not run
    call("POST", "/entities", rule(R_OFF, {"entity": REMOTE, "attribute": "upperPress"}, [{"entity": LIGHT, "set": {"level": 1}}], enabled=False))
    call("PATCH", f"/entities/{LIGHT}/attrs", {"level": prop(5)})
    call("PATCH", f"/entities/{REMOTE}/attrs", {"upperPress": prop(3)})
    waitFor("a disabled rule: not run (the 'on' rule sets 200)", lambda: attr(LIGHT, "level"), 200)

    # A rule deleted: its subscription goes
    call("DELETE", f"/entities/{R_TOG}")
    waitFor("a rule deleted: one event subscription left", lambda: len([x for x in ourSubs() if x[-1].isdigit()]), 1)
    call("PATCH", f"/entities/{LIGHT}/attrs", {"on": prop(True)})
    call("PATCH", f"/entities/{REMOTE}/attrs", {"lowerPress": prop(1)})
    time.sleep(0.5)
    check("a deleted rule: no longer fires", attr(LIGHT, "on"), True)

    # The listener takes only its own path
    try:
        req = urllib.request.Request(f"http://127.0.0.1:{PORT}/notify/wrong-secret/0", data=b"{}", method="POST")
        urllib.request.urlopen(req, timeout=3)
        check("a notification without the secret: 404", 200, 404)
    except urllib.error.HTTPError as e:
        check("a notification without the secret: 404", e.code, 404)

finally:
    cep.terminate()
    cep.wait(timeout=5)
    for id in made:
        call("DELETE", f"/entities/{id}")
    for id in ourSubs():
        call("DELETE", f"/subscriptions/{id}")
    log.close()

print(f"\n{checks - fails}/{checks} checks passed" + ("" if fails == 0 else f" - corCep's log: /tmp/corcep-test-{RUN}.log"))
sys.exit(0 if fails == 0 else 1)
