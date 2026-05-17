# Agent guidance

- Install `expect` and run tests locally with it enabled before concluding any task:
  - `sudo apt-get install -y expect` (or equivalent on your OS)
  - `make`
  - `make check`
- If you add or change tests, you still must run them locally (not only in CI).
- Keep this project tiny and powerful: optimize for a small binary and minimal code growth.
- Prefer compact, readable logic (including bitwise/state-machine style where appropriate) over large `if/else` forests.
- Do not use GCC-only extensions; code must remain portable C99 and compile with any C99 compiler.
- Split logical changes into separate commits.
- Every commit must be buildable on its own (`make` must pass after each commit).
- Write clear commit messages that explain both what changed and why.
