# corCep - an NGSI-LD complex event processor

corCep watches entities in an NGSI-LD context broker and acts on them: when a wall switch is pressed, a
light goes on. Its rules are NGSI-LD entities of type `Rule` in the same broker - made, changed and deleted
through the NGSI-LD API, applied at once. The design, and what comes after this first version:
[doc/design.md](doc/design.md).

It works with any NGSI-LD broker: the events come as notifications of NGSI-LD subscriptions, the actions go
out as NGSI-LD requests.

**Status: P2.5** - named events; patterns `any`, `seq`, `all`, `repeat`, `absent`, `window`, `supersedes`;
schedules `cron` and `at` (a clock time, sunrise, sunset); `where` filters; `if` conditions on other entities -
`all` / `any` groups, nested;
`set`, `toggle` and `step` (... `until`) actions, on one entity or a group; rule statistics; `POST /dry-run` - rules
replayed against made-up events, for the rule editors.

## Build

Built on the cor-libs, as coraine is - sibling checkouts (`../corRest`, `../corHttp`, `../corJson`, ...),
each built there:

```
make                  # ./corCep
make install          # /usr/local/bin/corCep (PREFIX=..., DESTDIR=...)
./corCep --version    # the version, and the commits it was built from
```

`COR_LIBS=<dir>` for cor-libs checked out elsewhere.

`make libcorcep.a` - the engine alone, as a library: rules, patterns, schedules, conditions, actions, statistics,
state. It reaches the broker through a `CepTransport` (`src/cepTransport.h`); the corCep binary is it on NGSI-LD over
HTTP - the base of coraine's in-process plugin to come ([doc/design.md](doc/design.md) § 9).

`packaging/libs.sh <dir>` clones the cor-libs (their `main`) into `<dir>` and builds them, in order.

`corCep --version` prints the version and the commit of corCep and of each cor-lib it was built from - the libs
have no releases of their own:

```
corCep 0.1.0
  corCep     e4f1ca336bcd
  corBase    773e6fe8fe2c
  ...
```

## Install

**The image** - `quay.io/seamware/corcep:<version>`, amd64 and arm64:

```
docker run --rm --network host quay.io/seamware/corcep:0.1.0 --broker http://localhost:1026
```

It runs as the user `corcep`, in `/var/lib/corcep` (mount a volume there for `--state`). Every option can also be
given as an environment variable, `CORCEP_<OPTION>`: `CORCEP_BROKER`, `CORCEP_PORT`, `CORCEP_TOKEN_COMMAND`, ...
`docker/Dockerfile` builds it from a checkout: `docker build -f docker/Dockerfile -t corcep .`

**The package** - `corcep`, a .deb for Ubuntu 26.04, 24.04 and Debian 13 (amd64, arm64), from the GitHub release or
the Seamware apt repository (the one coraine's packages are in - the same key and sources line):

```
sudo apt install corcep
```

- `/usr/bin/corCep`; the service `corcep` - enabled and started on install, as the user `corcep`, with
  `--state /var/lib/corcep/state.cor`; restarted on failure (a broker not up yet).
- The options in `/etc/default/corcep`, as `CORCEP_<OPTION>` lines - by default `CORCEP_BROKER=http://localhost:1026`,
  a coraine on the same host. After a change: `sudo systemctl restart corcep`.
- An upgrade restarts the service; a purge removes the state and the user.

`packaging/deb/build.sh <out>` builds the package in a container of the distribution;
`packaging/deb/test-install.sh <dir>` installs it in a clean one, runs it, purges it.

**A release** is a tag on `main`, `v<version>` - the version in `src/corCep.h`; `.github/workflows/packages.yml`
builds and tests both, pushes the image, makes the GitHub release with the packages and adds them to the apt
repository. A pull request touching the build builds and tests both, pushing nothing.

## Run

```
corCep --broker http://localhost:1026 [--port 7602] [--notify-url URL] [--tenant T]
       [--token-env VAR | --token-command CMD] [--broker-cert cert.pem] [--stats-interval PT10S]
       [--renew-interval PT1H] [--retry-for PT30S] [--queue-max N] [--state FILE]
       [--jwks URL|FILE [--issuer ISS] [--audience AUD] [--roles-claim PATH] [--editor-role ROLE] [--jwks-refresh PT5M]]
```

| Option | |
|---|---|
| `--broker`, `-b` | the broker's base URL (required) |
| `--port`, `-p` | the port the broker's notifications come to (default 7602) |
| `--notify-url`, `-nu` | the URL the broker reaches corCep at (default `http://127.0.0.1:<port>`) - a broker on another host needs it |
| `--tenant`, `-t` | the tenant (`NGSILD-Tenant`); default: the broker's default tenant |
| `--token-env`, `-te` | the environment variable holding a bearer token, sent as `Authorization` - never a command-line argument. Fixed: never renewed |
| `--token-command`, `-tc` | a command (`sh -c`) whose first line of output is the bearer token - run at start, again before the token expires, and when the broker answers 401. See [A broker that takes short-lived tokens](#a-broker-that-takes-short-lived-tokens) |
| `--broker-cert`, `-bc` | the broker's own certificate (PEM), trusted as it is - for a broker whose certificate no CA signed and whose name is not the one in `--broker`. See [A broker with a certificate of its own](#a-broker-with-a-certificate-of-its-own) |
| `--stats-interval`, `-si` | how often at most a rule's statistics are written to its Rule entity - seconds or ISO 8601 (default `PT10S`; 0: at once) - see [Statistics](#statistics) |
| `--renew-interval`, `-ri` | how often corCep's subscriptions are renewed - seconds or ISO 8601 (default `PT1H`, up to `P365D`; 0: never) - a broker that keeps a subscription's owner only for a while (coraine `--auth oidc`: `--oidcOwnerMaxAge`, 24 h) stops notifying once it runs out, without a word |
| `--queue-max`, `-qm` | event notifications waiting for the engine, at most (default 10000, 1 to 1000000; and 64 MiB of their bodies) - full, the oldest are dropped. See [A burst of events](#a-burst-of-events) |
| `--retry-for`, `-rf` | how long an action's PATCH is tried again when the broker does not answer, or answers 408, 429 or 5xx - seconds or ISO 8601 (default `PT30S`, up to `PT1H`; 0: never again); then dropped and counted. See [Actions on their way](#actions-on-their-way) |
| `--state`, `-st` | a file the patterns' state is kept in over a restart - matches half done, deadlines, window values, steps running (default: not kept). See [State over a restart](#state-over-a-restart) |
| `--jwks`, `-jw` | the JWK Set - a URL (`http://`, `https://`) or a file - that the rule editors' tokens are checked against. Turns `POST /dry-run` on; keys that cannot be read: corCep does not start. See [Dry run](#dry-run) |
| `--issuer`, `-is` | with `--jwks`: the `iss` a token must have (default: not checked) |
| `--audience`, `-au` | with `--jwks`: an `aud` a token must have (default: not checked) |
| `--roles-claim`, `-rc` | with `--jwks`: the claim holding the roles - a path, dot-separated (default `realm_access.roles`, Keycloak's); an array of strings, or one string of them, space-separated |
| `--editor-role`, `-er` | with `--jwks`: the role a token needs for `/dry-run` (default `ngsild-write`) |
| `--jwks-refresh`, `-jr` | with `--jwks` or `--author-jwks`: how often the keys are fetched again - seconds or ISO 8601 (default `PT5M`, up to `P1D`); at once too when a token names a key not in the set |
| `--pdp`, `-pd` | the policy decision point asked whether a rule's author may make each request - OPA's data API, e.g. `http://opa:8181/v1/data/policy/main/allow`. Turns the author's rights on: see [The author's rights](#the-authors-rights). Needs `--author-jwks` and `--author-issuer` |
| `--author-jwks`, `-aj` | with `--pdp`: the JWK Set (a URL or a file) the rule editor signs the rules' `author` with |
| `--author-issuer`, `-ai` | with `--pdp`: the `iss` a rule's `author` must have - the rule editor's |

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
| `else` | the actions when `when.if` does not hold - see [else](#else) |
| `then[].entity` | the target: one entity |
| `then[].type`, `then[].q` | the target: every entity of the type (matching the q, if given) - instead of `entity` |
| `then[].set` | `{ "<attribute>": <value>, ... }` - PATCHed as Properties |
| `then[].toggle` | `"<attribute>"` - a boolean: read, the other value PATCHed - per target |
| `then[].step` | `{ "attribute", "by", "every", "min", "max", "for" }` - a number changed `by` every `every` - see below |
| `then[].until` | with `step`: the event that stops it |
| `then[].create` | `{ "id"?, "type", "<attribute>": <value>, ... }` - an entity created (`POST /entities`), the values as Properties; no target besides. No `id`: corCep makes one each time, `urn:ngsi-ld:<type>:<the rule id's last part>-<ms>-<n>` - an alert, a log entry. One there already (409): `lastError` |
| `then[].delete` | `true` - the target deleted, one entity or every one of the group; or `"<id>"` - that entity (no `entity` besides). Gone already (404): done, no error |
| `${...}` | in `entity`, `q`, and the values of `set` and `create`: a value of the match - see [Values from the match](#values-from-the-match) |
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
| `{ "absent": "moving", "for": "PT3M", "after": "still" }` | 3 min after a `still`, unless a `moving` came in between - `still` starts the time (again), `moving` clears it; once, until the next `still`; not at the start. See below |
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

### `absent` ... `after`

A motion sensor that reports only changes - `motion` true while people move, false after its own hold time - sends
nothing while people stay. `absent` alone counts from the last event: from the last `true`, the lights would go off
in the middle of a meeting. With `after`, the time starts at the event that says "quiet now" and is cleared by the
one that says "someone here":

```json
{
  "events": [
    { "event": "moving", "entity": "urn:ngsi-ld:MotionSensor:room-2-14", "attribute": "motion", "where": "motion==true" },
    { "event": "still",  "entity": "urn:ngsi-ld:MotionSensor:room-2-14", "attribute": "motion", "where": "motion==false" }
  ],
  "pattern": { "absent": "moving", "for": "PT3M", "after": "still" }
}
```

| | |
|---|---|
| `still` | starts the time - again from now if it was running |
| `moving` | clears it: no fire, nothing until the next `still` |
| the start | nothing running - unlike `absent` alone, which counts from the start |
| over a restart | kept with `--state`, as any deadline |

The test `cep_pattern_absent_after` runs it.

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

**Groups** - AND, OR and parentheses across entities. A condition is a leaf as above, or a group:

| | holds when |
|---|---|
| `{ "all": [ <condition>, ... ] }` | every one holds - asked in order, stopped at the first that does not |
| `{ "any": [ <condition>, ... ] }` | one holds - asked in order, stopped at the first that does |
| `[ <condition>, ... ]` | as `all` |

Groups nest (16 deep at most). "The door opens, and it is dark, and (the alarm is off or someone is in)":

```json
"when": { "type": "JsonProperty", "json": {
  "entity": "urn:ngsi-ld:DoorSensor:front", "attribute": "contact", "where": "contact==false",
  "if": { "all": [
    { "entity": "urn:ngsi-ld:LightSensor:hall", "q": "illuminance<20" },
    { "any": [
      { "entity": "urn:ngsi-ld:Alarm:hq",    "q": "state==\"off\"" },
      { "entity": "urn:ngsi-ld:Building:hq", "q": "occupied==true" } ] } ] }
}}
```

The log names what did not hold: a leaf by its q, a group in words - `'if' any(state=="off" | occupied==true) does
not hold`. One condition within one entity is the q's own: `illuminance<20;occupied==true`, `a==1|b==2`, `(...)`.
Left out, with the reason: an empty group, `all` and `any` in one object, a group with other members
(`entity`, `q`, ...).

### else

`else`, beside `then` (a JsonProperty, a list of actions of the same kinds): what the rule does when its `if`
does not hold. The `if` is asked once, then exactly one list runs - `then` or `else`.

One switch, two lights in a meeting room - "more light": the first light on, or, if it is on already, the second:

```json
"when": { "type": "JsonProperty", "json": {
  "entity": "urn:ngsi-ld:WallSwitch:meeting", "attribute": "press",
  "if": { "entity": "urn:ngsi-ld:Light:a", "q": "on==false" } } },
"then": { "type": "JsonProperty", "json": [ { "entity": "urn:ngsi-ld:Light:a", "set": { "on": true } } ] },
"else": { "type": "JsonProperty", "json": [ { "entity": "urn:ngsi-ld:Light:b", "set": { "on": true } } ] }
```

Two rules with opposite `if`s on the same event do not do this: they race - the first one's PATCH can reach the
broker before the second one's `if` is asked, and both run.

- A rule with `else` that fires either way counts in `fired`; `firedElse` counts the times `else` ran. The log:
  `'if' on==false does not hold - else`, then `fired - else`.
- `else` without an `if`, or not a non-empty list: the rule is left out, with the reason.
- Actions are numbered on across both lists (`then`'s 1 and 2, `else`'s 3): `lastError`'s `action 3` is
  `else`'s first.
- With `--pdp`, the rule's signature covers `else` too.
- `/dry-run` shows the branch: `"else": true` and `why` on the fire.

The test `cep_rule_else` runs it.

`if` is asked of the broker, not watched: corCep keeps no copy of the other entity, and its updates are not events.

## Statistics

corCep writes each rule's statistics to its Rule entity, with `POST /entities/<rule>/attrs`:

| Attribute | |
|---|---|
| `fired` | the times the rule fired - the count goes on from the entity's own value after a restart |
| `droppedMatches` | open matches dropped because a rule had 32 for one entity already - written once there is one |
| `droppedValues` | window values dropped because a window held 10000 already - written once there is one |
| `firedElse` | of `fired`, the times the `if` did not hold and `else` ran - written once there is one; goes on from the entity's own value |
| `droppedActions` | PATCHes dropped - not sent within `--retry-for`, or 100 waiting for one entity already - since corCep started; written once there is one. See [Actions on their way](#actions-on-their-way) |
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

### A broker that takes short-lived tokens

`--token-command` runs a command that prints a token - a JWT signed by a key of corCep's own, a Keycloak client's
token (client credentials), whatever the broker takes. corCep runs it:

- at start - a command that fails, or prints nothing: corCep does not start;
- again before the token expires: a JWT, 60 s before its `exp`; a token that is no JWT, or has no `exp`, every 5 minutes;
- again when the broker answers 401 - and the request is made once more, with the new token.

A command that fails later is logged; the old token is kept, and the command is tried again 10 s later.

```
corCep --broker http://localhost:1026 \
       --token-command 'curl -s -d grant_type=client_credentials -u corcep:$SECRET https://keycloak/realms/r/protocol/openid-connect/token | jq -r .access_token'
```

`--token-env` and `--token-command`: one of them.

### A broker with a certificate of its own

`--broker-cert` pins the broker's certificate: the TLS connection is accepted when the broker shows exactly that
certificate (its SHA-256) - instead of the CA store, and the host name is not checked. Any other certificate: the
connection is refused. For a broker whose certificate is self-signed and names a host corCep does not use - a home
hub's broker, reached at `localhost`, its certificate made for `hub.local`:

```
corCep --broker https://localhost:1026 --broker-cert /etc/hub/tls/cert.pem --token-command '/usr/local/bin/hub-token'
```

Without `--broker-cert`, an `https://` broker's certificate is checked against the system's CA store and its name.

The test `cep_broker_auth` runs both against a broker of its own (HTTPS, OIDC with a JWK Set file - `broker.sh up` starts it).

## The author's rights

A rule acts with corCep's identity, but with `--pdp` only where its **author** may act - in a data space
connector, what the author's own requests to the broker would be allowed. Off without `--pdp`.

**Who the author is.** The rule editor signs each rule it saves: an `author` Property, a JWS (RS256 or ES256) by
`--author-issuer`, signed by a key of `--author-jwks`, whose payload holds the author's identity as the editor
knows it (in a data space connector: the author's `verifiableCredential`) and the rule itself:

```json
{ "iss": "https://editor.example", "sub": "alice", "verifiableCredential": { ... },
  "rule": { "when": { ... }, "then": [ ... ], "else": [ ... ] } }
```

With `--pdp`, a rule is left out (with the reason) when it has no `author`, the signature or issuer is not right,
or `"rule"` is not its `when`, `then` and `else` (when it has one) - a rule changed without the editor does not run. Compared as JSON:
member order and number form (`1` / `1.0`) do not matter. An `exp`, if the signature has one, is checked when the
rules are read. corCep never holds the author's access token.

**What the author may.** Before each request corCep makes for a rule - a `set`, `toggle`, `step`, `create`,
`delete`, each member of a group - it asks the PDP, as APISIX's `opa` plugin asks OPA:

```json
{ "input": { "type": "http",
             "request": { "method": "PATCH", "path": "/ngsi-ld/v1/entities/urn:ngsi-ld:Light:a/attrs",
                          "headers": { "authorization": "Bearer <the rule's author>", "content-type": "application/json" },
                          "query": {}, "body": "{\"on\":{\"type\":\"Property\",\"value\":true}}" } } }
```

`{ "result": true }` lets it go out; anything else - `false`, no result (OPA's undefined) - does not: the action
fails, the rule's `lastError` says so. A data space connector's policies (odrl-pap's Rego) decode the token's
payload without checking its signature (the PEP did; here corCep did), so the policies written for the author's
own requests decide the rule's.

- **When the rules are read**, each action on a fixed entity (`set`, `toggle`, `delete`, no `${...}`) is asked
  at once: a no is in `lastError` before the rule ever fires. A `toggle` is asked as it turns on.
- **A decision is kept a minute** - the same request, the same rule (its signature). A policy change takes up to
  a minute to reach a rule.
- **The PDP not answering** (no answer, not a 200): the request waits and is tried again as a broker failure is
  (`--retry-for`), then dropped - never sent unchecked.
- The path is the decoded one (`urn:ngsi-ld:...`, as APISIX's `uri`), with the broker's `/ngsi-ld/v1`.
- With `--broker-cert`, the PDP is reached over plain HTTP (it is pinned to the broker's certificate) - an OPA
  beside corCep.
- A rule's signature is no token for `/dry-run` (a 401), even with the editor's role in it; `/dry-run` does not
  ask the PDP.

The test `cep_author_rights` runs it, against a stand-in PDP (`test/funcTests/tools/ftPdp.py`).

## Values from the match

An action's target, q and values may name the match:

| | |
|---|---|
| `${entity}` | the entity the rule fired for (a schedule fires for none: the action fails) |
| `${<event>.entity}` | the entity of the latest `<event>` the rule saw - for a rule on a type, the latest of that entity |
| `${<event>.value}` | that event's value: the notified attribute's, any JSON |
| `${<event>.avg}` (`min`, `max`, `sum`, `count`) | a window rule's aggregate now - the rule's own (an `avg` window gives `avg`) |

```json
"then": [
  { "entity": "${entity}", "set": { "last": "${press.value}", "note": "pressed ${press.value} on ${press.entity}" } },
  { "type": "Light", "q": "room==\"${room.value}\"", "set": { "on": true } },
  { "create": { "type": "Log", "from": "${entity}", "value": "${press.value}" } }
]
```

- A string that is one `${...}` and nothing else becomes the value, its type kept: `"${press.value}"` of a press of
  3 is the number 3. In a longer string the value is written in as text.
- The values come from the devices - whoever can write an entity writes them - so they are checked first: into a
  `q`, a string only between quotes and without a quote or backslash in it (else a number or a boolean); into an
  entity id, nothing with a space, a quote, `<`, `>`, `{`, `}`, `|`, `\`, `^`. One that does not pass: the action
  fails, the rule's `lastError` says why, the other actions run.
- A name the rule cannot give - an event not in its pattern, another aggregate than the window's - leaves the
  rule out, with the reason.
- The latest values are kept in memory: after a restart, an event's values come with its next event.
- `/dry-run` resolves them the same way; a value that does not pass shows as `{ "failed": "..." }`.
- Not yet: another attribute of the event's entity (`${press.entity.roomId}`), and `${...}` in a `step`'s target.

The test `cep_action_vars` runs it.

## Actions on their way

A rule's `set`, `toggle` and `step` become PATCHes of the targets' attributes, `create` a POST, `delete` a DELETE.
corCep sends them from a thread of its own, so a broker slow or away holds up no pattern: events are matched,
deadlines fire, while the requests wait.

| | |
|---|---|
| order | a queue per target entity: two actions on one light go out in the order they were made - the second waits for the first; a `create`, a `set` and a `delete` of one entity, in that order. Other entities' do not wait |
| retried | when the broker does not answer, or answers 408, 429, 5xx: again after 0.25 s, 0.5 s, 1 s, ... (5 s at most between two), for `--retry-for` from when the action was made (default 30 s) |
| then | dropped - a light switched minutes late is worse than not switched: logged, the rule's `lastError` says `dropped, not sent within 30 s`, its `droppedActions` counts it |
| final | any other answer: a 404 (the entity is not there), a 400, ... - not retried; logged, `lastError` |
| full | 100 waiting for one entity: the oldest is dropped, counted |
| a stop | SIGTERM: corCep waits 2 s at most for the PATCHes waiting to go out; those left are logged, not kept |

A `toggle` reads the target's value when the rule fires: a PATCH of that entity still waiting is not in it yet.

The rule's `fired` counts a fire when its actions are made, not when they arrive; a failure comes after, in
`lastError`.

The test `cep_action_retry` runs it, against a proxy that fails on demand (`test/funcTests/tools/ftFlakyBroker.py`).

## A burst of events

The broker's notifications wait in a queue for the engine. It holds `--queue-max` event notifications (default
10000) and 64 MiB of their bodies at most. Full - a burst faster than the engine, a slow broker under the `if`s - the
**oldest** waiting are dropped to make room: an event handled late is worse than not handled, and the newest are
what the place is now.

```
2026-11-17 13:12:54.187 the event queue is full (3 notifications) - the oldest dropped till it drains
2026-11-17 13:12:55.199 the event queue has room again - 8 notifications dropped while it was full
```

Every notification is answered 204, dropped or not. An error would be worse: after a failed notification an NGSI-LD
broker skips the subscription's notifications for its endpoint's cooldown (§ 5.2.15 - coraine: 30 s by default),
every event of it lost, not only the burst's.

A rule reload and an action's failure report are never dropped; a reload already waiting is not queued twice.

The test `cep_queue_bound` runs it: `--queue-max 3`, the broker's GETs made slow, 12 presses at once.

## State over a restart

```
corCep --broker http://localhost:1026 --state /var/lib/corcep/state.cor
```

Without `--state`, a restart forgets what the patterns were in the middle of: the first press of a double press,
the time an absence started, a window's values, a dimmer's step. With it, corCep writes them to the file and reads
them back at start.

| | |
|---|---|
| written | at most once a second, when something changed; and at a stop (SIGTERM, SIGINT - corCep writes it and the statistics, then exits, within a second) |
| how | whole, to `<file>.tmp`, flushed to the disk, renamed over the file: a crash while writing leaves the last state written, never half of one. A crash loses the last second at most |
| format | corTree's binary format (cor), behind the 8 bytes `corCepS` + NUL |
| read | at start, before the rules: each rule's state found again by the rule's id |
| none yet, or not a state | corCep starts empty, and the log says so |

What comes back, and what does not:

| | after the restart |
|---|---|
| a match half done (`seq`, `all`, `repeat`) | goes on; its `within` counts from the first event, the time corCep was down included - a restart later than `within` drops it |
| a window's values | kept, each with its time; those older than `for` leave as they would have |
| an `absent` deadline still ahead | fires when it would have |
| an `absent` deadline that passed while corCep was down | not fired late: it starts again from the start |
| a superseded fire waiting (`supersedes`) whose time passed | dropped - fired late it would be worse than not fired |
| a step running (`step`) | goes on from the values it had reached, to its bound or its `for`; what it does (`by`, bounds, `every`, `until`) is read again from its rule. A rule, or a step of that attribute in it, gone meanwhile: it stops |
| a rule whose `when` changed meanwhile | starts afresh |
| `cron` / `at` | from the rule, as at any start |

The times are the wall clock's: the file is good after a reboot. Fire counts are not in it - they are on the Rule
entity ([Statistics](#statistics)).

The test `cep_state` runs it: a double press split by a stop, and by a `kill -9`; an absence's deadline over a stop,
and passed while down; a window; a step; a file that is not a state.

## Dry run

`POST /dry-run` replays rules against events made up for the purpose - what would fire, when, on which entities,
and what an `if` stopped. Nothing is written and the broker is not asked: the rule editors call it while a rule
is being written. The rules are parsed and run by the same code as the running ones, on a clock of the replay's own.

It is on with `--jwks` only (else 404), on corCep's `--port`. The caller sends its own token -
`Authorization: Bearer <token>`, a JWT (RS256 or ES256) signed by a key in the `--jwks` set, with `--issuer` /
`--audience` when given, and the `--editor-role` in its `--roles-claim`: no token or a bad one 401, no role 403.

The request:

| Member | |
|---|---|
| `rules` | Rule entities, keyValues - as `GET /entities?type=Rule&options=keyValues` gives them (`when` and `then` as `{"json": ...}`); 100 at most. A rule that does not parse is listed in the answer's `leftOut`, the others replayed |
| `events` | the events: `{"at", "entity", "type", "attribute", "value"}`; `at` - ms from the start, or an ISO 8601 duration (`PT0.5S`); `type` - needed for rules on a type; `value` - the attribute's value, or the attribute itself (`{"type": "Property", "value": ...}`); 10000 at most, replayed in time order |
| `entities` | the entities `where`, `if` and a group target's `q` are evaluated against - normalized or keyValues; each event changes its entity's attribute first |
| `start` | the wall clock at 0, ISO 8601 - for `cron` and `at` (default: now) |
| `for` | how long the replay runs, an ISO 8601 duration (default: the last event + `PT1H`, for the deadlines of `absent`, windows, `supersedes`); 31 days at most |

The answer:

| Member | |
|---|---|
| `fires` | each time a rule was done, in time order: `at` (ms from the start), `time`, `rule`, `entity` (the event's; `null` for a schedule), `fired` - `true` with the `actions` (each target entity named, a group's resolved against `entities`), or `false` with `why` (the `if` that did not hold) |
| `leftOut` | the rules not replayed: `rule`, `why` |
| `start`, `end` | the replay's start (ISO 8601) and end (ms) |
| `truncated` | `true`: stopped at 1000 fires |

What a replay does not do: actions are not applied - a light switched by one rule does not change the state the next
rule sees, and a `step` is reported, not stepped. `where`, `if` and group `q`s are evaluated by corNgsild's matcher
(the broker's) with the attribute names as written - no `@context`. A replay runs on the listener's thread: while it
runs, notifications wait (the limits above keep it short).

```
mkdir -p /tmp/ed
python3 test/funcTests/tools/ftJwt.py key --out /tmp/ed --name k1 --alg ES256
python3 test/funcTests/tools/ftJwt.py jwks --out /tmp/ed/jwks.json /tmp/ed/k1.jwk
corCep --broker http://localhost:1026 --jwks /tmp/ed/jwks.json &
TOKEN=$(python3 test/funcTests/tools/ftJwt.py sign --key /tmp/ed/k1.pem --alg ES256 --claims '{"realm_access": {"roles": ["ngsild-write"]}}')
curl -s -X POST localhost:7602/dry-run -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{
  "start": "2026-11-17T18:00:00Z",
  "rules": [{"id": "urn:ngsi-ld:Rule:hall", "type": "Rule",
             "when": {"json": {"entity": "urn:ngsi-ld:WallSwitch:hall", "attribute": "press"}},
             "then": {"json": [{"toggle": "on", "entity": "urn:ngsi-ld:Light:hall"}]}}],
  "events": [{"at": "PT1S", "entity": "urn:ngsi-ld:WallSwitch:hall", "attribute": "press", "value": "upper"}]
}'
```

```
{
  "start": "2026-11-17T18:00:00.000Z",
  "end": 3601000,
  "fires": [
    {
      "at": 1000,
      "time": "2026-11-17T18:00:01.000Z",
      "rule": "urn:ngsi-ld:Rule:hall",
      "entity": "urn:ngsi-ld:WallSwitch:hall",
      "fired": true,
      "actions": [
        {
          "toggle": "on",
          "entities": [
            "urn:ngsi-ld:Light:hall"
          ]
        }
      ]
    }
  ],
  "leftOut": [],
  "truncated": false
}
```

The tests `cep_dry_run` (the replay) and `cep_dry_run_auth` (the token, the errors) run it. `make DRY_RUN=0` builds
corCep without it - and without corNgsild, corJsonld and corHash (137 KB of a 395 KB binary).

## How it talks to the broker

- `urn:ngsi-ld:Subscription:corcep-rules` - every Rule entity, created, deleted, or with `when`, `then`, `enabled`
  or `name` changed: the rules are read again. The read failing (the broker not answering, or an error): the rules
  running are kept, and the read is tried again - after 1 s, doubling to 30 s - till it works. At start: no rules
  till then.
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

A second broker, for `cep_broker_auth` only, as a home hub runs one: HTTPS with a self-signed certificate for
another name and OIDC tokens only - port 4063, an image with `--https` (`CEP_AUTH_IMAGE`). `make functest` starts it
too (`test/funcTests/broker.sh up`), and makes its certificates, signing key, JWK Set and token script in
`test/funcTests/hub/` (`CEP_AUTH_DIR`); `down` removes them.

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
| `cep_action_retry` | a PATCH answered 503 retried, and the one after it on the same entity waits - in order; another entity's goes at once; no answer for longer than `--retry-for`: dropped, `droppedActions`, `lastError`; a 404 not retried |
| `cep_queue_bound` | a full event queue drops its oldest notifications, logged; all answered 204 - the broker counts no failure, no cooldown follows |
| `cep_action_create_delete` | `create` with an id made each time, and with one given (twice: 409, `lastError`); `delete` of a group, of one id (twice: no error); create, set, delete of one entity in one rule - in order |
| `cep_rule_else` | `else`: then when the `if` holds, else when not - one branch per match; `firedElse`; `else` without `if` and an empty `else` left out; `/dry-run` shows the branch |
| `cep_author_rights` | `--pdp`: unsigned, edited (an `else` too), foreign-key and wrong-issuer rules left out; a denied fixed target in `lastError` when read; a group written only where allowed; decisions kept; the PDP down - waited for, then written; a rule's signature refused by `/dry-run` |
| `cep_action_vars` | `${entity}`, `${<event>.entity}`, `${<event>.value}` (typed, and as text), a window's `${t.avg}`, a q made from an event; a quote into the q and a space into an id refused; bad names left out; `/dry-run` resolves them |
| `cep_state` | `--state`: a match half done, a window, an absence's deadline and a step over a stop and a crash; a deadline passed while down starts again; a file that is not a state |
| `cep_rule_stats` | `fired`, `firedAt`, `lastError` on the Rule entity; no reload from them; the count survives a restart |
| `cep_pattern_seq` | a double press within 1 s fires; 1.6 s apart does not, and the second press starts a new match |
| `cep_pattern_supersedes` | a double press fires only the double-press rule; a single press fires the other 1 s late |
| `cep_pattern_repeat` | the 3rd press within 2 s fires; the presses a match used are not used again |
| `cep_pattern_absent` | no event for 1 s fires - from the start too; each event moves the deadline; once per absence |
| `cep_pattern_absent_after` | `absent ... after`: started by `still`, cleared by `moving`, again from a second `still`; not at the start; over a restart |
| `cep_pattern_all` | two events in either order within 2 s |
| `cep_pattern_partition` | a pattern on a type matches per entity |
| `cep_pattern_left_out` | patterns that are not right, each with the reason |
| `cep_target_group` | `type` + `q` sets the room's lights only; `toggle` on a group turns each light the other way |
| `cep_action_step` | `step` while held; `until` (the release) stops it; held on, it stops at `max`; `for` stops it |
| `cep_rule_if` | `if` not holding: not fired, logged; holding: fired |
| `cep_rule_if_groups` | `all` with an `any` inside: the `all` stopped at its first not holding, the `any` holding by either member; `all` + `any` in one object left out |
| `cep_action_left_out` | actions and conditions that are not right, each with the reason |
| `cep_pattern_window` | `avg` and `max`: fired when the bounds start to hold, once; again after the window empties; a value not a number |
| `cep_pattern_window_expire` | a value leaving the window fires a rule with no event; `count` per entity for a type |
| `cep_window_dropped` | 10001 values: the oldest dropped, `droppedValues` written without a fire |
| `cep_window_left_out` | windows that are not right, each with the reason |
| `cep_schedule_cron` | six fields: every 2 s, on the second; five: the next weekday 07:00, as Python counts it |
| `cep_schedule_at` | a clock time fires once, on time; sunset at the Rule's location, an offset; a new location - a new sunset |
| `cep_schedule_left_out` | schedules that are not right, each with the reason |
| `cep_broker_auth` | a broker of its own, HTTPS (a self-signed certificate for another name) and OIDC tokens only: `--broker-cert` pins it (another certificate refused), `--token-command` - a refused first token renewed on the 401, a token renewed before it expires |

The tests on patterns over time wait for events to be processed rather than sleep: a "sentinel" rule on the same
event fires on every one, and corCep feeds every rule of an event, the patterns first, before the next event.

CI (`.github/workflows/ci.yml`) builds corCep against the cor libs' main and runs the functional tests on every
pull request.


### Valgrind

```
make valgrind           # the functional tests, every long-running corCep under valgrind - before every release
```

corCep built `-g -O0` into `obj/debug`, the suite run with `corTest --vt`: a test fails on a definite or indirect leak
or a memory error (valgrind's report per test: `/tmp/corValgrind.<test>.<pid>.vg`). The waits of the test functions
are ten times longer there; a test that crashes corCep on purpose (`kill -9`) drops that process's report.

2026-11-19: 47 of 47, no leaks, no errors.

### Coverage

```
test/funcTests/broker.sh up
make coverage           # coverage/index.html, line by line; a summary per file on the terminal
```

corCep built with `--coverage` into `obj/coverage` (from scratch each time; `./corCep` is left as it is), the
functional tests run on that binary, then gcovr (>= 8.3; `GCOVR=<path>`). It measures corCep's own `src/` - not the
cor-libs it links, which stay uninstrumented sibling checkouts. A corCep a test kills with `kill -9` writes no
counts.

2026-11-17, 47 tests: **88.3 % of the lines** (3277 of 3710), all 192 functions, 72.8 % of the branches. What is not
run is mostly error handling: out of memory, the broker answering an error, start-up option errors.

## License

corCep is licensed under the [Apache License 2.0](LICENSE) - Copyright 2026 Seamware.
Every source file carries an `SPDX-License-Identifier`.
