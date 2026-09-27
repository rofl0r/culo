#!/usr/bin/env python3
"""Count how many full-screen redraws culo emits for one pasted burst.

Drives the editor in a pty and feeds the burst as a single write, the
way a terminal delivers a middle-click paste.  Every editor_refresh()
emits the cursor-home sequence exactly once, so counting that sequence
in the output stream gives the repaint count for the burst.

Usage: paste_probe.py <editor> <file> <burst-bytes>

Prints "<redraws> <seconds>".  The redraw count is the number of
repaints between the end of the initial draw and the end of the burst.
Exits 1 on setup failure.
"""
import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time

HOME_SEQ = b"\x1b[H"
# Poll granularity while waiting for the editor to go quiet.
POLL = 0.1
# Give up after this many quiet polls, or after the hard cap.
QUIET_POLLS = 8
HARD_CAP_SEC = 120.0


class Editor:
    """The editor under a pty, with a repaint counter fed by HOME_SEQ."""

    def __init__(self, path, target):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(path, [path, target])
            os._exit(127)
        # Without a window size the pty reports 0x0, the editor refuses to
        # draw, and the repaint count means nothing.
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 40, 120, 0, 0))
        self.repaints = 0
        self.dead = False

    def pump(self, seconds):
        """Read pending output for the given time. Count repaints."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            readable, _, _ = select.select([self.fd], [], [], POLL)
            if not readable:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                self.dead = True
                return
            if not chunk:
                self.dead = True
                return
            self.repaints += chunk.count(HOME_SEQ)

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass
        try:
            os.waitpid(self.pid, os.WNOHANG)
        except ChildProcessError:
            pass


def main():
    if len(sys.argv) != 4:
        sys.stderr.write(__doc__)
        return 1
    editor_path, target, burst = sys.argv[1], sys.argv[2], int(sys.argv[3])

    editor = Editor(editor_path, target)
    try:
        # The first draw can take longer than any fixed sleep on a loaded
        # machine, so wait for it instead of guessing.
        waited = 0.0
        while editor.repaints == 0 and waited < HARD_CAP_SEC:
            editor.pump(POLL)
            waited += POLL
            if editor.dead:
                sys.stderr.write("editor exited before its first draw\n")
                return 1
        if editor.repaints == 0:
            sys.stderr.write("no first draw within the time cap\n")
            return 1

        start = editor.repaints
        t0 = time.monotonic()
        os.write(editor.fd, b"x" * burst)

        # A burst has one repaint at most, at its tail.  Wait for that
        # tail repaint, then for the stream to fall quiet.
        seen = 0
        quiet = 0
        while time.monotonic() - t0 < HARD_CAP_SEC:
            editor.pump(POLL)
            if editor.dead:
                sys.stderr.write("editor exited during the burst\n")
                return 1
            if editor.repaints > start:
                seen = editor.repaints - start
            if seen and editor.repaints == start + seen:
                quiet += 1
                if quiet >= QUIET_POLLS:
                    break
            else:
                quiet = 0
        elapsed = time.monotonic() - t0
        print("%d %.2f" % (editor.repaints - start, elapsed))
    finally:
        editor.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
