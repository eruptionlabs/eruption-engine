# Eruption Luau Debugger

Debug the Luau game scripts of Eruption Engine from VS Code.

1. In the Eruption editor: **Game > Install VS Code Debugger** (copies this folder to
   `~/.vscode/extensions`), then restart VS Code.
2. Open the `assets/scripts` folder in VS Code (the editor already writes
   `.vscode/launch.json` there).
3. Press **Play** in the editor, then **F5** in VS Code ("Debug game scripts").
4. Click the margin of a script to set a breakpoint. When it stops, the game freezes
   until you continue; step over, into and out, see the call stack, local variables,
   upvalues and the game variables (`vars`).

The editor listens on `127.0.0.1:4711` (change with `ERUPTION_DEBUG_PORT`, `0` turns it
off). While VS Code is connected, scripts run in the interpreter without
optimizations so every line and local variable can be seen.
