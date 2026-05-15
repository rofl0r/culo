# CULO Editor

CULO Editor is a minimalist text editor with syntax highlight,
copy/paste, search/replace, utf-8 support.
It tries to provide a replacement for GNU nano, providing the
most useful features using the same keybindings, but in a fraction
of the binary and code size, and without external dependencies
other than libc.
It's also heavily optimized for speed: opening huge text files
with millions of lines should be (almost) instant, as well as
navigating in and/or editing them.
The same holds true for huge javascript or json files that are
compressed into a single line.

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
    - M-6: Copy marked region
    - Ctrl-K: Cut marked text
    - Ctrl-C: Cancel selection
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

CULO Editor does not depend on external library (not even curses).
It uses fairly standard VT100 (and similar terminals) escape sequences.

## Acknowledge

CULO Editor is a fork of Mazu Editor, which in turn was inspired by excellent
tutorial [Build Your Own Text Editor](https://viewsourcecode.org/snaptoken/kilo/).

The name kilo served as the inspiration for CULO, because kilo is really an
unfinished PoS full of bugs crashing into your face.
The original idea was to use COOLO as name, but it's one char too much to type
so it was shortened to CULO.

## License

CULO Editor is freely redistributable under the BSD 2 clause license. Use of
this source code is governed by a BSD-style license that can be found in the
LICENSE file.
