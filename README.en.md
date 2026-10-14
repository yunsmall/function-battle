# Function Battle

A turn-based mathematical shooter on the Cartesian plane: two teams alternate turns entering
a function (or curve equation) under a time limit, and the projectile flies along that curve
from your position — penetrating, friendly-fire enabled, blasting craters into obstacles it
hits. The last team standing wins.

- **Authoritative C++ server**: asio TCP (native clients) + WebSocket (browser clients) in a
  single process; multi-room, 60 Hz authoritative simulation, AI thinking on a thread pool
- **raylib graphical client**: local-vs-AI (with built-in local server logic) + online play
  + spectating
- **Web client**: open the page in a browser and play (no extra process required); browser
  players and C++ players share rooms and can fight side by side (one JSON protocol — one
  WebSocket text frame = one message)
- Core logic (curves, world, AI, rooms, protocol) lives in a static library shared by server
  and clients, so behavior is identical everywhere

## Gameplay

**Curve placement rule (the core idea)**: the curve is translated so its parametric starting
point lands on your firing position — constants in the function do not affect the trajectory.
Example: from (2,10), entering `cos(x)` yields the actual path `y = cos(x−2) + 9`.

Seven curve modes:

| Mode | Form | Default parameter range | Direction |
|---|---|---|---|
| y = f(x) | single-variable function | map bounds | right (default) / left |
| x = f(y) | single-variable function | map bounds | up (default) / down |
| r = f(θ) | polar | 0 → 2π | increasing parameter |
| Parametric | (x(t), y(t)) | 0 → 1000 | increasing parameter |
| dy/dx = f(x,y) | ODE (RK4 integration) | — | along +x (default) / −x |
| (x', y') = (f, g) | ODE system (RK4 integration) | t from 0 | increasing parameter |
| y'' = f(x, y, dy) | second-order ODE (RK4) | — | launch angle adjustable (±85°) |

For parametric curves, t itself is unlimited; the only length constraint is
"distance traveled ≤ 2 × the map diagonal" (writing `(t, t)` draws until it leaves the map
or hits that length). Whatever leaves the map is cut off.

**Turns**: input phase (120 s by default) → submit → flight phase (the next turn starts once
the projectile is gone). The projectile travels along the curve at constant arc-length speed
(12 units/s by default). Hitting a soldier costs 1 HP (1 HP = one-shot kill by default); a
projectile resolves against each soldier once; **friendly fire is on**. Hitting an obstacle
destroys the projectile and blasts craters into every obstacle overlapping the explosion
(cratered regions no longer block later projectiles). Leaving the map or exceeding the travel
limit also destroys it.

**Map**: 48×30 by default (x∈[−24,24], y∈[−15,15]); obstacles (circles/rectangles) and spawn
points are derived from a seed — the same seed reproduces the exact same map.

**Expressions**: allowed functions and constants are listed in the in-game help panel; the
input is character-whitelisted and control flow is forbidden (pure math only).

**AI**: four difficulty levels (Easy / Normal / Hard / God). Lower levels miss aim and pick
random shapes; Hard/God plan multi-target interpolations (piercing several targets with one
shot, plus "tweak the coefficients and retry" mimicry) and always prefer curves that never
touch an obstacle — blasting obstacles open is a last resort when no clean solution exists.

**Room passwords** (optional): set at room creation. The password is hashed locally with
SHA-256 (plaintext never goes over the wire); the server only stores/compares the hash, never
broadcasts it, and room lists show only a "has password" flag.

## Architecture

```
fg_server (single process; both ports share one room table)
  9527  native protocol: length-prefixed JSON frames (C++ clients)
  8000  WebSocket text frames + static files (browser clients; same messages)
```

- Authoritative simulation: physics, hits and terrain changes all happen on the server;
  clients resample the same curve locally for smooth rendering
- Projectiles are broadcast at ~20 Hz; clients interpolate every frame with an
  "extrapolated target + smooth convergence" scheme (no teleporting; stops the moment the
  projectile is destroyed)
- Spectators can join anytime (in-progress games send a snapshot); everyone shares one
  broadcast stream
- Server ticks at 60 Hz; AI decisions run on a thread pool so they never block I/O

## Building

vcpkg dependencies: Eigen3, nlohmann-json, asio, cxxopts, exprtk (header-only), websocketpp,
gtest. raylib is pinned to the official prebuilt package in `third_party/raylib-5.5/`
(vcpkg's 6.0 shows a rendering regression on this machine). Example (clang + Ninja + vcpkg):

```bash
cmake -B build -GNinja \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build            # or: ninja -C build
```

Outputs: `build/src/server/fg_server.exe`, `build/src/client/fg_client.exe`,
`build/src/tests/fg_tests.exe`.

## Running

### Server

```bash
fg_server                            # defaults: game port 9527 + web port 8000
fg_server --port 9527 --web-port 8000 --seed 12345 --hp 3 --input-seconds 60
fg_server --web-port 0               # disable the web port (native protocol only)
fg_server --selftest 50              # no network: run 50 local AI-vs-AI games as a self-check
```

Command-line options double as the default configuration for newly created rooms (clients
can override them per room).

### C++ client

```bash
fg_client                            # GUI: main menu → local vs AI / online play
fg_client --selftest 10              # headless: 10 local games
fg_client --screenshot shot          # auto-play and capture screenshots (dev verification)
```

**Local vs AI**: choose team size, soldiers per player, AI difficulty, HP, obstacle count,
first turn and input time; you can also pick "role → spectator" to watch AI vs AI without
joining the lineup.

**Online**: main menu → online play → server address → connect. Create/join rooms in the
lobby (password-protected rooms need the password filled into the "join password" box);
in the room's ready screen the host can adjust the lineup (add AI, switch teams, kick) and
the "next match" configuration before starting.

### Web client

With the server running, just open `http://<server-host>:8000/` in a browser. Browser
players (including mobile) and C++ players share the same room table — you can fight in the
same room or mix teams freely.

**Spectating**: players and spectators are two distinct member kinds:

- The lobby room list reports "players" and "spectators" separately; the **Spectate** button
  always works — including mid-game (you receive a snapshot and continue from the current turn)
- In the room's ready screen anyone can switch themselves to spectator (not mid-game: lineups
  lock); spectators have no team, occupy a separate quota (20 slots), cannot submit curves,
  but appear in the member list
- Mid-game: player lineups are locked (no joins/leaves/team changes); spectators come and go
  freely. The spectator's game view has no self marker and a disabled control panel, with an
  "exit spectating" button in the top bar
- Pure AI spectating: the host switches to spectator and adds AI to both teams — the result
  is an AI-vs-AI match the host watches from start to finish

## Tests

```bash
fg_tests                             # gtest unit tests (curves / world / AI / rooms / protocol / hashing ...)
fg_server --selftest 100             # full-game self-check + same-seed reproducibility
fg_client --selftest 100             # headless full games on the client
```

## Repository layout

```
src/core/     shared core: config, expr (expression wrapper), curve sampling, world
              simulation, ai, room (members / match start / AI driving), protocol codec,
              sha256 (room passwords), error
src/server/   net (asio TCP/WS frame protocol), server (multi-room, 60 Hz tick dispatch),
              web_front (web access: WS + static files), main
src/client/   app (state machine), ui_* (screens), render, net_client, headless (tests/screenshots)
src/tests/    gtest unit tests
web/static/   web client (vanilla JS, no build step): app.js / render.js / net.js / path.js /
              sha256.js / strings.js
scripts/      fg_play.py: a file-mailbox game client (drop an expression into a file to submit;
              meant for scripts / AI assistants)
```

Protocol and error-code reference: [PROTOCOL.md](PROTOCOL.md) (Chinese).

## License

This project is open-sourced under the **GPL-3.0** (full text in [LICENSE](LICENSE)).

Third-party dependency licenses: Eigen3 (MPL-2.0), nlohmann-json (MIT), asio (Boost Software
License), cxxopts (MIT), websocketpp (BSD-3-Clause), gtest (BSD-3-Clause), raylib 5.5 (zlib;
prebuilt package under `third_party/`).

⚠ **exprtk (the expression parser) uses its own license: free for non-commercial use;
commercial use requires purchasing a license from its author.** Handle this dependency
(buy a license or swap the parser) before using this project commercially.
