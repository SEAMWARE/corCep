#!/bin/bash
#
# FILE            run.sh
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corCep's tests against a running NGSI-LD broker: BROKER_URL (default http://127.0.0.1:1026). The tests make
# entities and rules of their own (ids of the run) and delete them at the end.
#
DIR=$(cd "$(dirname "$0")" && pwd)
exec python3 -I "$DIR/check.py" "$DIR/../corCep" "${BROKER_URL:-http://127.0.0.1:1026}"
