# Mazu Editor

Mazu Editor is a minimalist text editor with syntax highlight, copy/paste, and search.

## Usage

Command line: (`filename` is optional)
* me `<filename>`

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
* Ctrl-N: Toggle line numbers display
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
* M-\\: Go to first line of file
* M-/: Go to last line of file
* PageUp, PageDown: Scroll up/down
* Up/Down/Left/Right: Move cursor
* Home/End: move cursor to the beginning/end of editing line

Mazu Editor does not depend on external library (not even curses). It uses fairly
standard VT100 (and similar terminals) escape sequences.

## Acknowledge

Mazu Editor was inspired by excellent tutorial [Build Your Own Text Editor](https://viewsourcecode.org/snaptoken/kilo/).

## License

Mazu Editor is freely redistributable under the BSD 2 clause license. Use of
this source code is governed by a BSD-style license that can be found in the
LICENSE file.
