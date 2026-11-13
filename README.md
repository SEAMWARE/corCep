# corCep - an NGSI-LD complex event processor

corCep watches entities in an NGSI-LD context broker and acts on them: when a wall switch is pressed, a
light goes on. Its rules are NGSI-LD entities of type `Rule` in the same broker - made, changed and deleted
through the NGSI-LD API, applied at once. The design, and what comes after this first version:
[doc/design.md](doc/design.md).

It works with any NGSI-LD broker: the events come as notifications of NGSI-LD subscriptions, the actions go
out as NGSI-LD requests.

**Status: P2.1** - named events, `any` of them, `where` filters, `set` and `toggle` actions, rule statistics.
Patterns over time (sequences, repetition, absence, windows) come next.

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
       [--stats-interval 10]
```

| Option | |
|---|---|
| `--broker`, `-b` | the broker's base URL (required) |
| `--port`, `-p` | the port the broker's notifications come to (default 7602) |
| `--notify-url`, `-nu` | the URL the broker reaches corCep at (default `http://127.0.0.1:<port>`) - a broker on another host needs it |
| `--tenant`, `-t` | the tenant (`NGSILD-Tenant`); default: the broker's default tenant |
| `--token-env`, `-te` | the environment variable holding a bearer token, sent as `Authorization` - never a command-line argument |
| `--stats-interval`, `-si` | seconds between two writes of a rule's statistics to its Rule entity (default 10; 0: at once) - see [Statistics](#statistics) |

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
| `when.where` | an NGSI-LD q: only updates of an entity matching it are events (`"level>50"`) - see below |
| `then` | the actions, run in order; one failing does not stop the others |
| `then[].entity` | the target |
| `then[].set` | `{ "<attribute>": <value>, ... }` - PATCHed as Properties |
| `then[].toggle` | `"<attribute>"` - a boolean: read, the other value PATCHed |
| `enabled` | a Property; `false`: the rule is not run |

A rule that is not right is left out (the log says why); the others run.

### Several events: `events` and `pattern`

```json
"when": { "type": "JsonProperty", "json": {
  "events": [
    { "event": "up",   "entity": "urn:ngsi-ld:WallSwitch:room-2-14", "attribute": "upperPress" },
    { "event": "down", "entity": "urn:ngsi-ld:WallSwitch:room-2-14", "attribute": "lowerPress" }
  ],
  "pattern": { "any": [ "up", "down" ] } } }
```

| Member | |
|---|---|
| `events` | the events, each `{ "event": <name>, "entity" or "type", "attribute", "where" }` - names unique in the rule (default: the attribute) |
| `pattern` | `"<name>"`: that event; `{ "any": [ <names> ] }`: any of them; absent: the one event of `events` |

The other patterns of [doc/design.md](doc/design.md) § 3.1 (`seq`, `all`, `repeat`, `absent`, `window`, schedules)
are recognised and refused for now: the rule is left out, "pattern 'seq' is not supported yet".

### `where`

`where` is the q of the event's subscription: the BROKER filters, so an update that does not match is never
notified and costs corCep nothing. Its syntax is NGSI-LD q, and its attributes are the watched entity's - not only
the watched attribute: `{ "entity": "urn:ngsi-ld:Dimmer:room-2-14", "attribute": "level", "where": "level>50" }`
fires on each update of `level` that leaves it above 50. A q the broker refuses: the subscription is not made, the
log says why (`POST subscription ...`).

## Statistics

corCep writes each rule's statistics to its Rule entity, with `POST /entities/<rule>/attrs`:

| Attribute | |
|---|---|
| `fired` | the times the rule fired - the count goes on from the entity's own value after a restart |
| `firedAt` | the last time it fired, a DateTime |
| `lastError` | the last action that failed and why; `observedAt` is when |

A rule's statistics are written at most once every `--stats-interval` seconds (default 10): a rule firing often costs
the broker one write per interval, not one per fire. `--stats-interval 0` writes after every fire:

```
corCep --broker http://localhost:1026 --stats-interval 0
curl -s 'localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Rule:room-2-14-upper?attrs=fired,firedAt&options=keyValues'
```

The Rule subscription watches only `when`, `then`, `enabled` and `name`: writing the statistics does not make
corCep read the rules again.

## How it talks to the broker

- `urn:ngsi-ld:Subscription:corcep-rules` - every Rule entity, created, deleted, or with `when`, `then`, `enabled`
  or `name` changed: the rules are read again.
- `urn:ngsi-ld:Subscription:corcep-<n>` - one per (entity or type, attribute, `where`) the rules watch. A subscription
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

| Test | |
|---|---|
| `cep_set_on_event` | a rule read at start: an update sets an attribute of another entity |
| `cep_rule_made_while_running` | a Rule made while corCep runs applies at once; a `set` of two attributes |
| `cep_toggle` | `toggle`; the same value reported again is an event too |
| `cep_rule_left_out` | a rule that is not right is left out, with the reason; the others run |
| `cep_rule_disabled` | `enabled: false` is not run |
| `cep_rule_deleted` | a deleted rule no longer fires; a subscription no rule needs is deleted |
| `cep_notify_secret` | notifications only on the listener's path with the secret |
| `cep_event_any` | `events` + `pattern`: `any` of two events fires the rule; a pattern naming one event uses that one |
| `cep_when_left_out` | a `when` that is not right is left out, with the reason |
| `cep_event_where` | `where` is the subscription's q: only matching updates fire |
| `cep_rule_stats` | `fired`, `firedAt`, `lastError` on the Rule entity; no reload from them; the count survives a restart |

CI (`.github/workflows/ci.yml`) builds corCep against the cor libs' main and runs the functional tests on every
pull request.
