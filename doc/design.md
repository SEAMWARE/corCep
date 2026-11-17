# corCep - design note

An NGSI-LD complex event processor: it watches entities in an NGSI-LD broker, recognises situations (one update,
a sequence, an absence, a value over a window) and acts on entities. Written in C on the cor-libs. Works with any
NGSI-LD broker; with coraine it can also run inside the broker.

Status: P1 and P2.1 to P2.5 built (README.md) - events, `where`, the patterns any / seq / all / repeat / absent /
window, the schedules cron / at, `supersedes`, `if`, set / toggle / step ... until on one entity or a group,
statistics, `/dry-run`. Section 8 lists the decisions, section 9 the releases.

## 1. What it is for

| Need | Example |
|---|---|
| An event switches something | a wall switch's upper button switches the room's lights on |
| A held button steps a value | held: the light brighter every 0.3 s until released |
| A sequence | two presses within 0.5 s: every light of the room off |
| A combination | a door opened AND the building in night mode: an alert entity |
| An absence | no presence in corridor 2 for 10 min: the corridor lights off |
| A window / aggregate | the mean temperature over 1 h above 26 °C: the fan on |
| A guard | the loading-dock door opens only on a double press, never on a single one |
| A schedule | every day at sunset: the car park lights on |
| Several actions | the conveyor stopped AND the line's warning beacon on |
| A group target | every Light in the room the switch is in |

Users: any NGSI-LD deployment - buildings, sites, industry (a FIWARE
Generic Enabler candidate: no NGSI-LD CEP exists in the FIWARE catalogue; Perseo is NGSIv2 and Orion only).

## 2. Architecture

```
          subscriptions (watchedAttributes, q)            PATCH / POST / DELETE
 broker  ──────────────── notifications ───────────▶  corCep  ──────────────────▶  broker
   ▲                                                     │
   └──────────── rules = NGSI-LD entities (type Rule) ◀───┘ read + subscribed to: a changed rule applies at once
```

- **Events in**: NGSI-LD subscriptions - one per watched (entity or type, attribute) - their notifications to
  corCep. `notificationTrigger` (entity/attribute updated, created, deleted) and `q` are the broker's: a rule's
  simple filter costs corCep nothing.
- **Actions out**: NGSI-LD operations on the broker (PATCH an attribute, create / delete an entity, a batch),
  plus an HTTP call and an NGSI-LD notification to a URL.
- **Rules**: entities of type `Rule` in the broker, the same API as the data; corCep subscribes to them, so a rule
  made in an app or c³ runs without a restart. A file of rules is an import into the broker, not a second source.
- **State**: the open matches (a first press waiting for a second, a timer, a window's values) in corCep's
  memory, written to a file so a restart does not lose them (section 6).
- **No broker change needed.** Everything above is NGSI-LD 1.8 API.

Two ways to run the same engine (one library, `libcorcep`):

| | standalone service `corcep` | coraine plugin `cep.so` |
|---|---|---|
| broker | any NGSI-LD broker | coraine only |
| events | notifications over HTTP | coraine's write path, in process - no notification hop |
| latency per event | one HTTP notification + one request back | a function call + the write |
| built | always | behind `COR_FEATURE_CEP` in coraine |

## 3. The rule model

A rule is an entity:

```json
{
  "id": "urn:ngsi-ld:Rule:room-2-14-double-press",
  "type": "Rule",
  "name": { "type": "Property", "value": "room 2.14: two presses - all room lights off" },
  "enabled": { "type": "Property", "value": true },
  "when": { "type": "JsonProperty", "json": { ... } },
  "then": { "type": "JsonProperty", "json": [ ... ] }
}
```

`when` and `then` are JsonProperties: their member names are not JSON-LD terms (no expansion, no @context),
the rule reads the same in any context.

### 3.1 `when` - events, conditions, patterns

An **event** is one notification of one watched attribute, the same value again included (a button pressed twice):

```json
{ "event": "press", "entity": "urn:ngsi-ld:WallSwitch:room-2-14", "attribute": "upperPress" }
```

| Member | |
|---|---|
| `event` | the name the pattern uses |
| `entity` / `type` + `q` | which entities: one id, or every entity of a type that matches q (NGSI-LD q, parsed by corNgsild) |
| `attribute` | the attribute watched |
| `where` | a condition on the notified value or other attributes of the same entity (NGSI-LD q syntax) - the q of the event's subscription: the broker evaluates it (built in P2.1) |

A rule with one event writes it as `when` itself (`{ "entity", "attribute", "where" }`); with several, `when` is
`{ "events": [ <event>, ... ], "pattern": <pattern> }` (built in P2.1).

A **condition on other entities** (the state of the site, not an event): `"if": { "entity" | "type", "q" }`, or
a list of them (all must hold), asked of the broker when the pattern completes - `GET /entities?id=...&q=...&limit=1`,
an entity found: it holds (built in P2.3). Asked, not watched: corCep keeps no copy of the site, the broker is the
truth, and an `if` costs one request per fire, not a subscription per entity.

A **pattern** combines events. The vocabulary is SQL:2016 row pattern recognition (`MATCH_RECOGNIZE`,
ISO/IEC 9075-2:2016) - sequence, alternatives, quantifiers - in JSON:

| Pattern | Means |
|---|---|
| `"press"` | one event |
| `{ "seq": ["press", "press"], "within": "PT0.5S" }` | two presses, the second within 0.5 s of the first |
| `{ "any": ["a", "b"] }` | a or b |
| `{ "all": ["doorOpen", "nightMode"], "within": "PT1M" }` | both, in any order, within 1 min |
| `{ "repeat": "press", "min": 3, "within": "PT2S" }` | three presses within 2 s |
| `{ "absent": "motion", "for": "PT10M" }` | no motion event for 10 min (a timer, restarted by each event) |
| `{ "absent": "moving", "for": "PT3M", "after": "still" }` | 3 min after `still` unless `moving` came (the timer started by one event, cleared by another - for a sensor that reports only changes) |
| `{ "window": "temp", "for": "PT1H", "avg": { "gt": 26 } }` | the mean over the last hour above 26 (also `min`, `max`, `sum`, `count`) |
| `{ "at": "sunset" }` / `{ "cron": "0 7 * * 1-5" }` | a schedule (sunset at the Rule entity's `location`; the host's local time) - built in P2.5 |

Durations are ISO 8601 (as NGSI-LD's own). A `seq` that finishes also cancels the shorter rules it overlaps,
when the rule says so (`"supersedes": ["urn:ngsi-ld:Rule:room-2-14-press"]`): a double press does not also fire
the single press - at the cost of waiting `within` before the single press fires (section 8, decision 4).

### 3.2 `then` - actions

A list, run in order:

| Action | |
|---|---|
| `{ "set": { "on": true }, "entity": "<id>" }` | PATCH the attributes |
| `{ "toggle": "on", "entity": "<id>" }` | a boolean: read, the other value PATCHed |
| `{ "step": { "attribute": "level", "by": 25, "min": 1, "max": 254, "every": "PT0.3S", "for": "PT30S" }, "until": "release" }` | add `by` every `every` until the event `until`, min/max (every target at one), or `for` (default PT30S) - built in P2.3 |
| `"entity"` / `"type"` + `"q"` | the target: one entity, or every entity of a type matching q, found when the action runs (a group) - built in P2.3 |
| `{ "create": { ...entity... } }` / `{ "delete": "<id>" }` | an alert entity, a log entry |
| `{ "http": { "method": "POST", "url": "...", "body": ... } }` | a call out (a webhook) |
| `{ "notify": "<url>" }` | an NGSI-LD notification of the match |

Values in an action may name the match: `"${press.entity}"`, `"${temp.avg}"`, `"${press.entity.roomId}"` (not built
yet - a group target's q is a literal today).

## 4. Built from standards

| Piece | Standard |
|---|---|
| events | NGSI-LD subscriptions and notifications (ETSI GS CIM 009) |
| filters, conditions | NGSI-LD q (the same grammar, the same parser - corNgsild) |
| patterns | SQL:2016 row pattern recognition concepts (ISO/IEC 9075-2) |
| durations, schedules | ISO 8601 durations; cron syntax |
| rules | NGSI-LD entities, JsonProperty |
| actions | NGSI-LD API operations; HTTP |

Considered and not taken as the rule language (each a whole engine or tied to one): Esper EPL (Perseo's core,
Java), Drools DRL (Java), OMG DMN / FEEL (decision tables - no time, no sequences), CloudEvents CESQL (filters
only, no patterns), W3C RIF (no time), RSP-QL (RDF streams, SPARQL - heavy for an edge gateway), Home Assistant
automations and Node-RED flows (product formats). None is widely adopted for CEP.

## 5. The engine

- **Per event**: find the rules whose events match (an index by entity id and attribute, then type), evaluate
  `where`, feed each rule's pattern automaton.
- **Patterns** compile to a small NFA per rule; a partial match is an NFA instance with its start time. `within`,
  `for` and `absent` are timers (one timer wheel). Bounded: at most N partial matches per rule (oldest dropped,
  counted in the rule's statistics).
  Built in P2.2 (src/pattern.c): seq / all keep open matches (at most 32 per rule and entity), repeat the times of
  the last events, absent a deadline; `within` is checked when an event comes, and the deadlines (absent, a
  superseded fire waiting) are found by a scan on each turn of the loop - the loop sleeps until the next one.
  A scan is enough for hundreds of rules; the timer wheel waits for a measurement that says otherwise.
- **Windows** keep running aggregates (sum, count, min/max by a monotonic deque) - not every value.
  Built in P2.4: a ring of (time, value) for sum / avg / count, the deque for min / max, at most 10000 values per
  window and partition (the oldest dropped, counted: `droppedValues`). Fires on the EDGE - when the bounds start to
  hold - looked at on each value and when the oldest value leaves (a deadline, as absent's). The time window
  slides; tumbling windows ("the mean of each hour") are not built.
- **Entity state** for `if` and group targets: none - both are asked of the broker when the rule fires (P2.3).
  A `step` keeps the values it writes, read once from the targets when it starts; it runs on the loop's deadlines,
  as the patterns' timers do.
- **Actions** go out on an ordered queue per target entity: two actions on one light are never reordered.
- **Threads**: one event loop (notifications, timers, the broker's answers); the actions' PATCHes on a thread of
  their own (src/sender.c), one request at a time, the target due first.

## 6. State, restart, failure

- Partial matches, deadlines, window values and steps: written to `--state` (corTree's binary format) at most once
  a second when they changed, and at stop (SIGTERM); a restart within a match's `within` continues it, a later
  restart drops it. A deadline passed while down: an absence starts again, a superseded fire is dropped - nothing
  fires late. A step goes on with what its rule says now. Built: README "State over a restart".
- The subscriptions: corCep's own (id prefix `urn:ngsi-ld:Subscription:corcep-`); at start the stale ones are
  removed, every 5 min any missing one is made again.
- The broker away: actions are retried with back-off for `--retry-for` (default 30 s), then dropped and counted -
  a light switched minutes late is worse than not switched.
- Per rule, as attributes of the Rule entity (written at most every `--stats-interval` s, default 10): `firedAt`,
  `fired` (count), `lastError`, `droppedMatches` (with the patterns over time). The Rule subscription watches
  `when`, `then`, `enabled`, `name` only - the statistics written are not news to corCep (built in P2.1). The app shows "what it does" and "when it last did it".

## 7. Security

- corCep acts with its own identity: an ES256 key, a token with the roles the rules need (`ngsild-read`,
  `ngsild-write`), the broker's JWK Set holding its public half. The token comes from `--token-command` - signed
  outside corCep, renewed before it expires and on a 401; corCep never holds the key.
- A broker whose certificate is its own (a home hub's, self-signed, reached at `localhost`): `--broker-cert` pins
  it - that certificate and no other, instead of the CA store and the host name.
- A rule acts with corCep's rights, but only where its author may act: corCep checks the author's rights on each
  target - when the rule is saved and again when it fires (a group's members change). A target the author may not
  write is not written; the rule's `lastError` says so. Who may write a Rule entity at all is the broker's normal
  access control - a Rule is an entity like any other (section 8, decision 6). Works against any broker.
- Notifications come to a path holding a secret made at start; loopback, or TLS.
- `http` actions: to an allow-list of hosts only.

## 8. Decisions

1. **The pattern language** - SQL:2016 row pattern recognition concepts, written as JSON (this note). Decided
   2026-11-17: the Seamware team was asked, no one answered; the rules already run on it and the editors write it.
2. **Rules as entities in the same broker** (this note) - the same API, the same access control, the same backup.
3. **The plugin** - yes: `cep.so` for coraine as well as the standalone service (decided 2026-11-17) - events from
   coraine's write path, actions as function calls: faster, and the shape a home hub wants. In v1.1 (section 9).
4. **Single vs double press** - per rule: `supersedes` (built in P2.2).
5. **Scope** - section 9 (redrawn 2026-11-17: windows and schedules were built before v1).
6. **Who may write a rule** - the broker's normal access control for writing the Rule entity; corCep checks the
   author's rights on every target (section 7). Decided 2026-11-17.
7. **ETSI** - the rule model as an NGSI-LD proposal: later, not urgent (private material until decided).
8. **The repo** - `SEAMWARE/corCep`, private until v1 (decided 2026-11-13); licence Apache 2.0, as coraine (KZ 2026-11-15).

## 9. Releases

**v1** - the first public release (the GE application):

| | |
|---|---|
| built | events, `where`; any / seq / all / repeat / absent / window; cron / at; `supersedes`; `if` (all / any); set / toggle / step ... until on one entity or a group; Rule statistics; broker auth (`--token-command`, `--broker-cert`); subscriptions renewed; `/dry-run` (the caller's token against `--jwks`; a pure replay - `where`, `if` and groups against the request's entities, corNgsild's q matcher); `--state` (matches, deadlines, windows and steps over a restart); the actions on a thread of their own, a queue per target, retried for `--retry-for` with back-off, then dropped and counted (`droppedActions`); the event queue bounded (`--queue-max`) - full, the oldest dropped; never an error to the broker, whose cooldown would skip the subscription for 30 s |
| to build | `create` / `delete` actions; `${...}` match values in actions; the author's rights checked per target (section 7) |
| packaging | a version tag and `--version`, a Docker image, the docs, the repo public |

**v1.1** - `cep.so`: the engine as a library, a coraine write-path hook (under its own `COR_FEATURE` flag),
`src/` split into the engine and its transport (notifications in, HTTP out).

**v2** - `http` actions (webhooks, an allow-list of hosts); `notify` actions; tumbling windows; a timer wheel,
if a measurement asks for one; the ETSI proposal. Candidates: windows across entities (the mean of every room),
rule templates (one rule for every button of a type), a `/metrics` endpoint.
