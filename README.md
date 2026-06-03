# CPP-BD - Shadowlight Duel

`Shadowlight Duel` is a C++ 2D asymmetric online PVP demo built for the course project. The first playable version implements the base idea from the design brief: Light Blade versus Shadow Walker, realtime network synchronization, skills, health, score, capture point, cooldown UI, and win settlement.

## Current Features

- Win32/GDI 2D window, no external art or game-engine dependency.
- TCP LAN PVP with one host and one client.
- Offline local duel mode for quick presentation and testing.
- Light Blade: ranged light attack, burst light skill, place light beacon.
- Shadow Walker: ranged shadow attack, dash strike, shadow field to weaken lights.
- Realtime synchronized state: player positions, health, projectiles, scores, lights, capture progress, match phase.
- Game loop, hit detection, cooldowns, respawn, first to 3 score win condition.
- Start menu, room waiting state, HUD, cooldown display, capture bar, victory screen.

## Controls

Menu:

- `H`: host room as Light Blade.
- `J`: join room as Shadow Walker.
- `O`: offline local duel.
- Type digits and dots to edit the join IP, `Backspace` deletes.

Light Blade:

- `W/A/S/D`: move.
- `F`: attack.
- `G`: burst light.
- `Q`: place light beacon.
- `R`: reset match.

Offline Shadow Walker:

- Arrow keys: move.
- `Enter`: attack.
- Right `Shift`: dash.
- Right `Ctrl`: shadow field.

Client Shadow Walker:

- `W/A/S/D`: move.
- `F`: attack.
- `G`: dash.
- `Q`: shadow field.
- `R`: reset match.

## Build With MinGW

From the repository root:

```powershell
mingw32-make
```

Run:

```powershell
.\build\ShadowlightDuel.exe
```

If `mingw32-make` is unavailable, compile directly:

```powershell
g++ -std=c++14 -O2 -Wall -Wextra "CPP-BD/src/main.cpp" -o "build/ShadowlightDuel.exe" -mwindows -lws2_32 -lgdi32
```

## Visual Studio

Open:

```text
CPP-BD/CPP-BD.sln
```

The project references `CPP-BD/src/main.cpp` and links `ws2_32.lib` for networking.

## Network Demo

1. On the first computer, run the game and press `H`.
2. On the second computer, run the game, type the host IP, and press `J`.
3. The host controls Light Blade, the client controls Shadow Walker.

For same-machine testing, open two copies, host one copy, keep `127.0.0.1` in the second copy, then join.

