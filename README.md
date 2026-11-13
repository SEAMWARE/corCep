# corCep - an NGSI-LD complex event processor

corCep watches entities in an NGSI-LD context broker and acts on them: when a wall switch is pressed, a
light goes on. Its rules are NGSI-LD entities of type `Rule` in the same broker - made, changed and deleted
through the NGSI-LD API, applied at once. The design, and what comes after this first version:
[doc/design.md](doc/design.md).

It works with any NGSI-LD broker: the events come as notifications of NGSI-LD subscriptions, the actions go
out as NGSI-LD requests.

**Status: P1** - one event per rule, `set` and `toggle` actions. Patterns over time (sequences, absence,
windows) are P2.

## Build

Built on the cor-libs, as coraine is - sibling checkouts (`../corRest`, `../corHttp`, `../corJson`, ...),
each built there:

```
make                  # ./corCep
make install          # /usr/local/bin/corCep (PREFIX=..., DESTDIR=...)
```

`COR_LIBS=<dir>` for cor-libs checked out elsewhere.

## Run

```
corCep --broker http://localhost:1026 [--port 7602] [--notify-url URL] [--tenant T] [--token-env VAR]
```

| Option | |
|---|---|
| `--broker`, `-b` | the broker's base URL (required) |
| `--port`, `-p` | the port the broker's notifications come to (default 7602) |
| `--notify-url`, `-nu` | the URL the broker reaches corCep at (default `http://127.0.0.1:<port>`) - a broker on another host needs it |
| `--tenant`, `-t` | the tenant (`NGSILD-Tenant`); default: the broker's default tenant |
| `--token-env`, `-te` | the environment variable holding a bearer token, sent as `Authorization` - never a command-line argument |

The options can also come from the environment: `CORCEP_BROKER`, `CORCEP_PORT`, ...

The log goes to stderr: the rules loaded (and any left out, with the reason), each rule fired, each action
that failed.

## A rule

```json
{
  "id": "urn:ngsi-ld:Rule:room-2-14-upper",
  "type": "Rule",
  "name": { "type": "Property", "value": "room 2.14: upper button - light on" },
  "when": { "type": "JsonProperty", "json": { "entity": "urn:ngsi-ld:WallSwitch:room-2-14", "attribute": "upperPress" } },
  "then": { "type": "JsonProperty", "json": [ { "entity": "urn:ngsi-ld:Light:room-2-14-desk", "set": { "on": true } } ] }
}
```

| Member | |
|---|---|
| `when.entity` | the entity watched - its type taken from the id (`urn:ngsi-ld:<type>:...`), or `when.type` |
| `when.type` | without `when.entity`: every entity of the type |
| `when.attribute` | the attribute watched: each update of it is an event - the same value again too (a button pressed twice) |
| `then` | the actions, run in order; one failing does not stop the others |
| `then[].entity` | the target |
| `then[].set` | `{ "<attribute>": <value>, ... }` - PATCHed as Properties |
| `then[].toggle` | `"<attribute>"` - a boolean: read, the other value PATCHed |
| `enabled` | a Property; `false`: the rule is not run |

A rule that is not right is left out (the log says why); the others run.

## How it talks to the broker

- `urn:ngsi-ld:Subscription:corcep-rules` - every Rule entity, created, updated and deleted: the rules are read
  again.
- `urn:ngsi-ld:Subscription:corcep-<n>` - one per (entity or type, attribute) the rules watch. A subscription
  stays as long as a rule needs it - a rule added or removed does not touch the others' subscriptions, so no
  event is lost while the rules are read again.
- At start, the `corcep-*` subscriptions of an earlier run are deleted.
- The notifications come to `<notify-url>/notify/<secret>/<n>`; the secret is made at start - nothing else can
  send corCep an event.

## Test

```
make functest
```

The functional tests, `test/funcTests/cases/*.test`, run by corTest (the cor stack's test runner). `make functest`
starts a broker for the run - a coraine container (`quay.io/seamware/coraine:0.7.0`, ramDB) on the host's
network, port 4062 - and stops it at the end. Each test starts its own corCep and works in a tenant of its own,
new on every run. Against another NGSI-LD broker: `CEP_BROKER_HOST=... CEP_BROKER_PORT=... corTest`.

`make test` runs the older checks (`test/check.py`) against a broker already running (`BROKER_URL`, default
`http://127.0.0.1:1026`): set, toggle (the same value twice), a rule made while running, a bad rule beside good
ones, a disabled rule, a rule deleted, a notification without the secret. They move to corTest test cases one by
one.

CI (`.github/workflows/ci.yml`) builds corCep against the cor libs' main and runs the functional tests on every
pull request.
