# culo editor

culo is a terminal text editor with syntax highlighting,
copy/paste, search/replace, utf-8 support, undo/redo, line-ending
detection, file browser, help screen, etc.
It tries to provide a replacement for GNU nano, providing the
most useful features using the same keybindings, but in a fraction
of the binary and code size, and without external dependencies
other than libc.
It's also heavily optimized for speed: opening huge text files
with millions of lines should be (almost) instant, as well as
navigating in and/or editing them.
The same holds true for huge javascript or json files that are
compressed into a single line.

## Screenshot

<img width="1299" height="772" alt="culo" src="https://github.com/user-attachments/assets/63311288-258f-431a-b15f-b6e8b7fa88bd" />

## Usage

Command line: (`filename` is optional)
* culo `<filename>`

Supported keys:
* Ctrl-X: Quit (prompts to save if modified)
* Ctrl-O: Save file
* Ctrl-Z: Undo
* Ctrl-Y: Redo (also M-E)
* Ctrl-W: Find string in file
    - ESC to cancel search, Enter to exit search, arrows to navigate
    - M-C: Toggle case sensitivity
    - M-B: Toggle backwards search
    - M-R: Toggle regex mode
    - Ctrl-R: Toggle replace mode
* Ctrl-K: Cut current line (consecutive Ctrl-K appends to cut buffer)
* Ctrl-U: Paste/uncut
* Ctrl-G: Show help screen
* M-A: Set/toggle mark (text selection)
    - Move cursor to select text while marking
    - Shift+Arrow/Home/End/PgUp/PgDn: transient marking while held; first non-shift-cursor key clears selection
    - M-6: Copy marked region
    - Ctrl-K: Cut marked text
    - Ctrl-C: Cancel selection
* Shift+Cursors can be used too to select text.
* M-B: Open file browser
    - Arrow keys to navigate files and directories
    - Enter to open file or enter directory
    - ESC or Ctrl-X to cancel
* M-G: Go to line number
    - Type line number, Enter to jump, ESC to cancel
* M-]: Go to matching bracket
* M-\\: Go to first line of file
* M-/: Go to last line of file
* M-#: Toggle line numbers display
* M-P: Toggle whitespace/tab marker display
* M-Y: Toggle syntax highlighting
* PageUp, PageDown: Scroll up/down
* Up/Down/Left/Right: Move cursor
* Home/End: move cursor to the beginning/end of editing line

Syntax highlighting is auto-disabled for recognized files larger than 16 MiB.

culo does not depend on external library (not even curses).
It uses fairly standard VT100 (and similar terminals) escape sequences.

## Acknowledge

culo is a fork of Mazu Editor, which in turn was inspired by the excellent
tutorial [Build Your Own Text Editor](https://viewsourcecode.org/snaptoken/kilo/).

The name kilo served as the inspiration for culo.
The original idea was to use "coolo" as name, but it's one char too much to
type so it was shortened, but it is pronounced like coolo.

special thanks goes to @github for letting me use @copilot PRO for free.
being able to use claude sonnet and GPT codex was a tremendous help for
turning my idea into a reality.

## License

culo is freely redistributable under the BSD 2 clause license. Use of
this source code is governed by a BSD-style license that can be found in the
LICENSE file.
