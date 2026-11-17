#!/usr/bin/env python3
#
# FILE            ftPdp.py
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# A policy decision point for corCep's --pdp, as OPA's data API answers: POST { "input": ... } -> { "result": bool }.
#
#   ftPdp.py <port> <policy file> <log file>
#
# The input is what APISIX's opa plugin hands OPA: input.request.method, .path, .headers.authorization (a bearer
# JWT), .body. Like a data space connector's Rego, the token's payload is decoded WITHOUT checking its signature.
# A request is allowed when a line of <policy file> - "<sub> <METHOD> <path prefix>" - matches the token's "sub",
# the method and the start of the path. A line "DOWN": every request is a 503 - the PDP not answering. The file is
# read at each request. Each decision is logged, one line: "<sub> <METHOD> <path> <yes|no>".
#
import base64
import http.server
import json
import socketserver
import sys

port, policyFile, logFile = int(sys.argv[1]), sys.argv[2], sys.argv[3]


def lines():
    try:
        with open(policyFile) as f:
            return [line.split() for line in f if line.strip()]
    except OSError:
        return []


def subOf(authorization):
    try:
        payload = authorization.split(" ", 1)[1].split(".")[1]
        payload += "=" * (-len(payload) % 4)
        return json.loads(base64.urlsafe_b64decode(payload)).get("sub", "-")
    except (IndexError, ValueError):
        return "-"


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):                                # the start's probe
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        policy = lines()

        if ["DOWN"] in policy:
            self.send_response(503)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        request = json.loads(body)["input"]["request"]
        sub = subOf(request["headers"].get("authorization", ""))
        method, path = request["method"], request["path"]
        yes = any(len(p) == 3 and p[0] == sub and p[1] == method and path.startswith(p[2]) for p in policy)

        with open(logFile, "a") as f:
            f.write("%s %s %s %s\n" % (sub, method, path, "yes" if yes else "no"))

        answer = json.dumps({"result": yes}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(answer)))
        self.end_headers()
        self.wfile.write(answer)


socketserver.TCPServer.allow_reuse_address = True
with socketserver.ThreadingTCPServer(("127.0.0.1", port), Handler) as server:
    server.serve_forever()
