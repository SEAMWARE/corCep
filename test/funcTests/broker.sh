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
IMAGE="${CEP_BROKER_IMAGE:-quay.io/seamware/coraine:0.7.0}"
PORT="${CEP_BROKER_PORT:-4062}"
NAME="corcep-functest-broker-$PORT"

case "$1" in
  up)
    docker rm -f "$NAME" > /dev/null 2>&1
    docker run -d --rm --name "$NAME" --network host "$IMAGE" --database ramDB --troe none --port "$PORT" > /dev/null || exit 1

    for ((i = 0; i < 100; i++)); do
      curl -s -o /dev/null "http://127.0.0.1:$PORT/ngsi-ld/v1/types" && { echo "broker: $IMAGE on port $PORT"; exit 0; }
      sleep 0.1
    done

    echo "broker.sh: $IMAGE does not answer on port $PORT after 10s:" >&2
    docker logs "$NAME" 2>&1 | tail -20 >&2
    exit 1
    ;;

  down)
    docker rm -f "$NAME" > /dev/null 2>&1
    exit 0
    ;;

  *)
    echo "usage: broker.sh up|down" >&2
    exit 1
    ;;
esac
