# corCep - an NGSI-LD complex event processor

corCep watches entities in an NGSI-LD context broker and acts on them: when a wall switch is pressed, a
light goes on. Its rules are NGSI-LD entities of type `Rule` in the same broker - made, changed and deleted
through the NGSI-LD API, applied at once. The design, and what comes after this first version:
[doc/design.md](doc/design.md).

It works with any NGSI-LD broker: the events come as notifications of NGSI-LD subscriptions, the actions go
out as NGSI-LD requests.

**Status: P2.5** - named events; patterns `any`, `seq`, `all`, `repeat`, `absent`, `window`, `supersedes`;
schedules `cron` and `at` (a clock time, sunrise, sunset); `where` filters; `if` conditions on other entities;
`set`, `toggle` and `step` (... `until`) actions, on one entity or a group; rule statistics. `/dry-run` comes next.

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
| `when.if` | conditions on OTHER entities, asked of the broker when the rule would fire - see below |
| `then` | the actions, run in order; one failing does not stop the others |
| `then[].entity` | the target: one entity |
| `then[].type`, `then[].q` | the target: every entity of the type (matching the q, if given) - instead of `entity` |
| `then[].set` | `{ "<attribute>": <value>, ... }` - PATCHed as Properties |
| `then[].toggle` | `"<attribute>"` - a boolean: read, the other value PATCHed - per target |
| `then[].step` | `{ "attribute", "by", "every", "min", "max", "for" }` - a number changed `by` every `every` - see below |
| `then[].until` | with `step`: the event that stops it |
| `enabled` | a Property; `false`: the rule is not run |
| `location` | a GeoProperty, a Point: where the sun of an `at: "sunrise"` / `"sunset"` rule is |

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
| `pattern` | `"<name>"`: that event; an object, below; absent: the one event of `events` |
| `supersedes` | with `seq`, `all`, `repeat`: the ids of single-event rules this one supersedes - see below |

### Patterns

| Pattern | Fires |
|---|---|
| `{ "any": [ "a", "b" ] }` | on each `a` and each `b` |
| `{ "seq": [ "press", "press" ], "within": "PT0.5S" }` | when the steps come in this order, the last within 0.5 s of the first - a double press |
| `{ "all": [ "door", "here" ], "within": "PT1M" }` | when each step has come, in any order, within 1 min |
| `{ "repeat": "press", "min": 3, "within": "PT2S" }` | at the 3rd press within 2 s |
| `{ "absent": "motion", "for": "PT10M" }` | when no `motion` has come for 10 min - once, until the next `motion`; the time counts from the start too |
| `{ "window": "temp", "for": "PT1H", "avg": { "gt": 26 } }` | when the mean of the last hour's values STARTS to be above 26 - see below |
| `{ "cron": "0 7 * * 1-5" }` | at 07:00 on weekdays - a schedule, no events - see below |
| `{ "at": "sunset", "offset": "-PT30M" }` | every day half an hour before sunset - see below |

Durations are ISO 8601, as NGSI-LD's own: `PT0.5S`, `PT10M`, `PT1H`, `P1DT2H`.

How a pattern matches - SQL:2016 row pattern recognition's defaults:

- an event goes to the oldest open match that can take it; an event a match took starts no new one
- a match done clears the other open matches: the events it used are not used again
- a rule whose events are all type selectors (`"type": "WallSwitch"`, no `entity`) matches per entity - two
  switches' presses are not a double press. A rule naming entities by id has one match state.
- an event's time is when its notification reaches corCep
- at most 32 open matches per rule and entity; beyond, the oldest is dropped and counted (`droppedMatches`)
- a rule's open matches and timers survive a reload of the rules if its `when` is the same; a changed `when`
  starts afresh

### `window`

A window is the values of one event over the last `for` - the notified attribute's values, numbers:

```json
{ "window": "temp", "for": "PT1H", "avg": { "gt": 26 } }
```

| Member | |
|---|---|
| `window` | the event - its attribute's values are the window's |
| `for` | an ISO 8601 duration: how far back the window goes |
| `avg`, `min`, `max`, `sum`, `count` | the aggregate - one of them, with its bounds |
| `gt`, `ge`, `lt`, `le` | the bounds: `>`, `>=`, `<`, `<=` a number - one or more, all must hold (`{ "gt": 20, "lt": 30 }`: between) |

- The rule fires when the bounds START to hold - once; again only after they have stopped holding. A mean above
  26 for an hour is one fire, not one per value.
- The window is looked at on each value AND when a value leaves it: the bounds can start to hold with no event at
  all - `{ "max": { "lt": 30 } }` fires when the last value of 30 or more leaves the window.
- An empty window: `avg`, `min`, `max` and `sum` do not hold; `count` is 0.
- A value that is not a number is not in the window; the log says so.
- At most 10000 values per window (per entity, for a type selector); beyond, the oldest is dropped and counted
  (`droppedValues`) - the window is then shorter than `for`.
- `sum`, `avg` and `count` keep the values; `min` and `max` only those that can still be the min or max.

### Schedules: `cron` and `at`

A schedule fires the rule at a time - no events, so `events` is not needed:

```json
"when": { "type": "JsonProperty", "json": { "pattern": { "cron": "0 7 * * 1-5" } } }
```

| Pattern | Fires |
|---|---|
| `{ "cron": "<minute> <hour> <day> <month> <weekday>" }` | as cron: `*`, `n`, `a-b`, lists `a,b`, steps `*/15`, `a-b/2`; months `JAN`-`DEC` and weekdays `SUN`-`SAT` by name too; weekday 0-7 (0 and 7: Sunday). Day of month and weekday both given (neither `*`): either one fires - as cron |
| `{ "cron": "<second> <minute> <hour> <day> <month> <weekday>" }` | six fields: the second first (`"*/10 * * * * *"`: every 10 s) |
| `{ "at": "07:00" }`, `{ "at": "07:00:30" }` | every day at that time |
| `{ "at": "sunrise" }`, `{ "at": "sunset" }` | every day when the sun's upper edge is at the horizon, at the Rule entity's `location` - within a minute |
| `"offset": "-PT30M"` | with `at`: that much later - `-` before it for earlier |

- The times are the host's local time (its `TZ`), daylight saving included. A time that daylight saving skips is
  skipped that day; a time it repeats fires once.
- The log says each rule's next time - at start, at each fire, after a change: `rule ...: next 2026-11-16 07:00:00 +0100`.
- corCep not running at a rule's time: that fire is not made up for.
- A sunrise / sunset rule needs `location` on the Rule entity - a GeoProperty, a Point (`[longitude, latitude]`).
  A change of it moves the rule's sun: corCep watches `location` as it watches `when`. Where the sun does not rise
  or set for a while (polar night, midnight sun), the next time is the next day it does.
- A schedule that never fires (`"0 0 30 2 *"`) is left out.
- A schedule takes `if` as any rule: `"if": { "entity": "urn:ngsi-ld:Building:hq", "q": "occupied==true" }` - the
  lights at 07:00 on a working day only if someone is in.

### `supersedes`

A double-press rule and a single-press rule on the same button would both fire on a double press. `supersedes`
settles it:

```json
"when": { "type": "JsonProperty", "json": {
  "events": [ { "event": "press", "entity": "urn:ngsi-ld:WallSwitch:room-2-14", "attribute": "upperPress" } ],
  "pattern": { "seq": [ "press", "press" ], "within": "PT0.5S" },
  "supersedes": [ "urn:ngsi-ld:Rule:room-2-14-upper" ] } }
```

Each fire of `room-2-14-upper` now waits 0.5 s: a second press in that time is a double press, and
`room-2-14-upper` does not fire for either press. The cost: a single press acts 0.5 s late.

### `where`

`where` is the q of the event's subscription: the BROKER filters, so an update that does not match is never
notified and costs corCep nothing. Its syntax is NGSI-LD q, and its attributes are the watched entity's - not only
the watched attribute: `{ "entity": "urn:ngsi-ld:Dimmer:room-2-14", "attribute": "level", "where": "level>50" }`
fires on each update of `level` that leaves it above 50. A q the broker refuses: the subscription is not made, the
log says why (`POST subscription ...`).

### Group targets

An action's target is one `entity`, or a `type` with an optional `q`: every entity of the type that matches,
found when the action runs (`GET /entities?type=...&q=...`, at most 1000) - a light added to the room is in the
group from then on, with no change to the rule.

```json
{ "type": "Light", "q": "roomId==\"room-2-14\"", "set": { "on": true } }
```

`toggle` reads each target and writes it the other value: a group of lights half on, half off is turned the other
way round, each on its own.

### `step` ... `until`

`step` changes a number by `by` every `every` - dimming while a button is held:

```json
"when": { "type": "JsonProperty", "json": {
  "events": [
    { "event": "hold",    "entity": "urn:ngsi-ld:Remote:room-2-14", "attribute": "hold" },
    { "event": "release", "entity": "urn:ngsi-ld:Remote:room-2-14", "attribute": "release" }
  ],
  "pattern": "hold"
}},
"then": { "type": "JsonProperty", "json": [
  { "entity": "urn:ngsi-ld:Light:room-2-14-desk",
    "step": { "attribute": "level", "by": 20, "every": "PT0.2S", "max": 100 },
    "until": "release" }
]}
```

| `step` member | |
|---|---|
| `attribute` | the number changed; each target's own value is where its step starts - a target without a number is left out (logged) |
| `by` | the change of each step, not 0; negative steps down |
| `every` | an ISO 8601 duration, `PT0.05S` or more; the first step is at once |
| `min`, `max` | optional bounds: the value is held to them; when every target is at a bound, the step stops |
| `for` | an ISO 8601 duration, default `PT30S`: the step stops after it, whatever else - a lost `release` cannot run it for ever |

`until` names an event of the rule's `events` that is NOT in its pattern (an event of the pattern would fire the
rule again): when it comes, the step stops where it is. The rule firing again while its step runs starts the step
over from the targets' values then - one step per rule, target and attribute. The log says when and why a step
stops: `step of level stopped - at its bound`, `- its time limit`.

### `if`

`if` is a condition on another entity, checked when the rule would fire - "the light on only while the building is
occupied":

```json
"when": { "type": "JsonProperty", "json": {
  "entity": "urn:ngsi-ld:WallSwitch:room-2-14", "attribute": "upperPress",
  "if": { "entity": "urn:ngsi-ld:Building:hq", "q": "occupied==true" }
}}
```

`entity` or `type`, and `q`, as a target. It holds when the broker has an entity that matches (`GET
/entities?id=...&q=...&limit=1`). A list of them: all must hold. A condition that does not hold: the rule is not
fired, not counted, and the log says so (`'if' occupied==true does not hold - not fired`). An error asking the
broker: taken as not holding, logged.

`if` is asked of the broker, not watched: corCep keeps no copy of the other entity, and its updates are not events.

## Statistics

corCep writes each rule's statistics to its Rule entity, with `POST /entities/<rule>/attrs`:

| Attribute | |
|---|---|
| `fired` | the times the rule fired - the count goes on from the entity's own value after a restart |
| `droppedMatches` | open matches dropped because a rule had 32 for one entity already - written once there is one |
| `droppedValues` | window values dropped because a window held 10000 already - written once there is one |
| `firedAt` | the last time it fired, a DateTime |
| `lastError` | the last action that failed and why; `observedAt` is when |

A rule's statistics are written at most once every `--stats-interval` seconds (default 10): a rule firing often costs
the broker one write per interval, not one per fire. A drop is written too, the rule fired or not.
`--stats-interval 0` writes after every change:

```
corCep --broker http://localhost:1026 --stats-interval 0
curl -s 'localhost:1026/ngsi-ld/v1/entities/urn:ngsi-ld:Rule:room-2-14-upper?attrs=fired,firedAt&options=keyValues'
```

The Rule subscription watches only `when`, `then`, `enabled`, `name` and `location`: writing the statistics does not
make corCep read the rules again.

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
| `cep_pattern_seq` | a double press within 1 s fires; 1.6 s apart does not, and the second press starts a new match |
| `cep_pattern_supersedes` | a double press fires only the double-press rule; a single press fires the other 1 s late |
| `cep_pattern_repeat` | the 3rd press within 2 s fires; the presses a match used are not used again |
| `cep_pattern_absent` | no event for 1 s fires - from the start too; each event moves the deadline; once per absence |
| `cep_pattern_all` | two events in either order within 2 s |
| `cep_pattern_partition` | a pattern on a type matches per entity |
| `cep_pattern_left_out` | patterns that are not right, each with the reason |
| `cep_target_group` | `type` + `q` sets the room's lights only; `toggle` on a group turns each light the other way |
| `cep_action_step` | `step` while held; `until` (the release) stops it; held on, it stops at `max`; `for` stops it |
| `cep_rule_if` | `if` not holding: not fired, logged; holding: fired |
| `cep_action_left_out` | actions and conditions that are not right, each with the reason |
| `cep_pattern_window` | `avg` and `max`: fired when the bounds start to hold, once; again after the window empties; a value not a number |
| `cep_pattern_window_expire` | a value leaving the window fires a rule with no event; `count` per entity for a type |
| `cep_window_dropped` | 10001 values: the oldest dropped, `droppedValues` written without a fire |
| `cep_window_left_out` | windows that are not right, each with the reason |
| `cep_schedule_cron` | six fields: every 2 s, on the second; five: the next weekday 07:00, as Python counts it |
| `cep_schedule_at` | a clock time fires once, on time; sunset at the Rule's location, an offset; a new location - a new sunset |
| `cep_schedule_left_out` | schedules that are not right, each with the reason |

The tests on patterns over time wait for events to be processed rather than sleep: a "sentinel" rule on the same
event fires on every one, and corCep feeds every rule of an event, the patterns first, before the next event.

CI (`.github/workflows/ci.yml`) builds corCep against the cor libs' main and runs the functional tests on every
pull request.
