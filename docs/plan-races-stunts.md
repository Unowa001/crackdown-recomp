# Selected check expansion: races and unique stunt rings

Owner selection, 2026-10-13: rooftop races, road races, and unique stunt rings.
This records the selected expansion; adapters, counts, IDs and logic are not yet
implemented or validated. The overnight implementation window remains stopped.

## Check contract

| Group | Completion that awards a check | AP option |
|---|---|---|
| Rooftop races | First successful completion of each distinct course | Include rooftop races |
| Road races | First successful completion of each distinct course | Include road races |
| Unique stunt rings | First legitimate completion of each distinct ring | Include unique stunt rings |

Expose independent toggles. Each location is observed once in the bound seed/save.
Starting, approaching, crossing an intermediate checkpoint, failing, timing out,
or retrying a race does not award a check. A personal-best improvement is not a
separate check. Generic vehicle stunts outside unique rings are not part of this
selection. Existing completions from an unrelated save/profile are not imported.

Persist observations before sending them; preserve pending checks offline;
reconcile native completion state after a bound load where the game stores it.
If a group lacks native saved completion flags, document and test its sidecar
recovery policy rather than treating it as equivalent to the orb save contract.
Keep the game's ordinary course/ring completion behavior; randomized skill XP
must continue to follow the existing suppression rules.

## Discovery evidence and implementation order

Read-only string discovery in the supported guest image found:
- `cRooftopRaceMission`, `cRooftopRaceProfile`, `cRooftopRaceCheckPoint` and
  `nRoadRace` / `IsRoadRace`.
- `eTriggerEvent_FinishedRooftopRace` at `820CEC40`,
  `eTriggerEvent_FinishedRoadRace` at `820CEC84`, and generic finished/started/
  timeout/personal-best race event names nearby.
- `StuntCompleted` at `8208624C`, `cUniqueStuntManager.cpp` at `820B3408`, and
  `eTriggerEvent_NearStuntRing` at `820CEE18`.

These identify investigation paths, not proven hooks. In order:
1. Enumerate courses/rings and their native definition IDs, type and positions.
   Prove identity across New Game, native load and streaming order. Determine
   actual counts from the supported game data; do not assign guessed totals.
2. Trace successful and failed completion paths. Capture the course/ring identity
   at the native completion callback, not from the nearest map marker.
3. Determine per-player ownership and native saved completion fields. Test
   retries, reloads, deaths, offline play and co-op player attribution.
4. Add stable location IDs to the shared C++/Python catalog without renumbering
   existing locations. Excluded groups retain their IDs. Set option defaults
   when the adapters and playtests support enabling them.
5. Add AP regions/rules based on verified course access, movement/vehicle needs
   and completion requirements. Do not infer rules solely from altitude or
   assume a vehicle/model requirement from a race's name.
6. Validate pool balance and enabled/disabled combinations, then demonstrate
   one successful live check from each group and no duplicates on replay/load.

Candidate priority: rooftop races, road races, then unique stunt rings. Resume
implementation only when the user resumes project work; this selection does not
restart the expired overnight window or mark the existing M2 runtime complete.
