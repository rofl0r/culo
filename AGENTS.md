# Agent guidance

- Run tests locally before concluding any task:
  - `make`
  - `make check`
  - Tests are C harness tests under `tests/harness/`; no `expect`
    or other external driver is needed.
- If you add or change tests, you still must run them locally (not only in CI).
- Keep this project tiny and powerful: optimize for a small binary and minimal code growth.
- Prefer compact, readable logic (including bitwise/state-machine style where appropriate) over large `if/else` forests.
- Do not use GCC-only extensions; code must remain portable C99 and compile with any C99 compiler.
- Split logical changes into separate commits.
- Every commit must be buildable on its own (`make` must pass after each commit).
- Write clear commit messages that explain both what changed and why.
- After each commit, inspect its diff; if you see duplicate code, refactor it.

## Debugging terminal apps without a visible screen

The editor draws to stdout and reads keystrokes from stdin, so you can drive
and inspect it entirely from a script.  Techniques that worked here:

- **Read-interposition harness.**  Compile the editor source into your test
  binary (`#define main culo_main`, then `#include "culo.c"`) and override
  `read()` via `dlsym(RTLD_NEXT, "read")`.  Feed scripted bytes to stdin and
  return EOF (0) when done.  This runs the real key decoder and main loop,
  not a mock.
- **Byte-exact expectations.**  After driving input, compare `row->chars`
  directly or dump the file with `cmp`/`xxd`.  "Looks right" is not a test;
  a hexdump of the saved file is.
- **Render to a string, eyeball later.**  Capture what `ui_draw_rows` would
  emit (or dump `row->highlight[]` codes) and map codes to ANSI escapes in
  the harness.  You can count distinct colors without a terminal.
- **Instrument, don't guess.**  When state looks impossible, print it at
  every step: cursor, mode flags, undo `count`/`cursor`, per-key byte feeds.
  One loop that prints `mode/active/shift_only` after each key beats an hour
  of reasoning.
- **Watch for hidden state machines.**  Escape decoders keep static flags
  (`meta_pending`) across calls; a keypress may consume bytes from the NEXT
  logical key.  Trace actual bytes consumed per iteration.
- **Differential builds catch allocator bugs.**  A heap overread invisible
  under glibc shows up instantly under musl + valgrind
  (`musl-gcc ... && valgrind -q --error-exitcode=42 ./test`).  Keep both
  toolchains in your harness rotation.
- **C trap: hex escapes eat following hex digits.**  `"\x1be"` is ONE char
  (0xBE), not ESC+'e'.  Use octal (`"\033e"`) or string concatenation
  (`"\x1b" "e"`).  This silently corrupts any harness feeding escape keys.
