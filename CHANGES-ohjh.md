# Fork changes (ohjihoon05)

Built on lpflhh/Pluto-AI-Starcraft-Remaster @ 44736dd. Original Pluto binaries are not included; get them from
https://github.com/tscmoo/pluto/releases (cog2026-2578600) and point `-Pluto` at `pluto.dll`.

## New launcher flags (`scripts/start-pluto.ps1`)

| Flag | Effect |
|---|---|
| `-Attach` | Inject into the x86 StarCraft the Battle.net app already started (keeps the Battle.net login). Enable "Launch 32-bit client" in the app's StarCraft game settings first. |
| `-Opponents a,b` | Multiplayer whitelist of opponent account names; empty = everyone. In `-AnyGame` every human must be listed. |
| `-AnyGame` | Admit any game type and player mix (UMS, FFA, Top vs Bottom, teams). Allied units are removed from Pluto's view; resignation is suppressed. |
| `-NoResign` | Ignore Pluto's own gg/leaveGame (default on with `-AnyGame`). |
| `-Opening free` | Seed the per-opponent bandit file so Pluto plays without a build hint (mask 0x0). |
| `-MaxUnits N` | Cap the units Pluto observes (nearest own, enemy, neutral first). ~400 halves inference on crowded maps. |
| `-ShowMessages` | Show the bridge's Korean notices in game (off by default; everything is still in `bridge.log`). |

## Bridge behaviour

- `src/unit_order.h`: active unit list is ordered own → other players → neutral, each by distance from the start
  location, so Pluto's 768-unit truncation and the 1700-slot table lose far neutrals first. Over-capacity cuts
  instead of stopping the AI.
- `src/legacy_view.cpp`: the three legacy unit lists are rebuilt from kept units only (hidden owners dropped).
- `src/bridge.cpp`: pins the process CWD to the runtime folder (logs/bandit files land there even when attached),
  publishes `BWRL_*` settings into the game's msvcrt environment before `pluto.dll` loads, reads the new
  `bridge.ini` keys (`opponents`, `any_game`, `no_resign`, `opening`, `max_units`, `show_messages`, `draw`,
  `straddle`, `frame_budget_ms`).
- Tests: `session-admission` (whitelist, any-game), `unit-ordering` (new), `korean-messages`.

## Not changed

Pluto model, weights, inference engine. Fastest-map economy is not adapted (would need retraining).
