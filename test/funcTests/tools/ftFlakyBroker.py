#!/usr/bin/env python3
#
# FILE            ftFlakyBroker.py
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# A broker that fails when a test says so: a proxy in front of the real one, for corCep's --broker.
#
#   ftFlakyBroker.py <port> <broker base URL> <fail file> <log file>
#
# Every request is passed on to the broker, except a PATCH /ngsi-ld/v1/entities/<id>/attrs whose <id> is a line of
# <fail file> - "<id> <status>": answered <status> itself (0: the connection closed, no answer at all). A line
# "GET <ms>": every GET is passed on <ms> late - a slow broker. A line "RULES <status>": the rules' GET
# (/ngsi-ld/v1/entities?type=Rule...) answered <status> itself. The file is read at each
# request: a test fails and heals the broker by writing the file. Each PATCH of attributes is logged, one line:
# "<id> <body, compact> <status>".
#
import http.server
import json
import socketserver
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

port, broker, failFile, logFile = int(sys.argv[1]), sys.argv[2].rstrip("/"), sys.argv[3], sys.argv[4]


def failing(entity):
    try:
        with open(failFile) as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2 and parts[0] == entity:
                    return int(parts[1])
    except OSError:
        pass
    return None


class Proxy(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def any(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length > 0 else None
        path = urllib.parse.urlsplit(self.path).path
        entity = None

        if self.command == "PATCH" and path.startswith("/ngsi-ld/v1/entities/") and path.endswith("/attrs"):
            entity = urllib.parse.unquote(path[len("/ngsi-ld/v1/entities/"):-len("/attrs")])
            status = failing(entity)
            if status is not None:
                self.log(entity, body, status)
                if status == 0:
                    self.close_connection = True
                    self.connection.close()
                    return
                self.send_response(status)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

        if self.command == "GET" and path == "/ngsi-ld/v1/entities" and "type=Rule" in self.path:
            status = failing("RULES")
            if status is not None:
                answer = b'{"type":"https://uri.etsi.org/ngsi-ld/errors/InternalError","title":"made to fail","status":%d}' % status
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(answer)))
                self.end_headers()
                self.wfile.write(answer)
                return

        if self.command == "GET":
            delay = failing("GET")
            if delay:
                time.sleep(delay / 1000)

        headers = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "content-length", "connection")}
        req = urllib.request.Request(broker + self.path, data=body, method=self.command, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                status, rbody, rheaders = r.status, r.read(), r.getheaders()
        except urllib.error.HTTPError as e:
            status, rbody, rheaders = e.code, e.read(), e.headers.items()

        if entity is not None:
            self.log(entity, body, status)

        self.send_response(status)
        for k, v in rheaders:
            if k.lower() not in ("content-length", "connection", "transfer-encoding", "date", "server"):
                self.send_header(k, v)
        self.send_header("Content-Length", str(len(rbody)))
        self.end_headers()
        self.wfile.write(rbody)

    def log(self, entity, body, status):
        text = json.dumps(json.loads(body), separators=(",", ":"), sort_keys=True) if body else "-"
        with open(logFile, "a") as f:
            f.write("%s %s %d\n" % (entity, text, status))

    do_GET = do_POST = do_PATCH = do_DELETE = do_PUT = any


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


Server(("127.0.0.1", port), Proxy).serve_forever()
