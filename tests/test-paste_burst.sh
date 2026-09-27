#!/usr/bin/env bash

# Test that a pasted burst costs one repaint, not one repaint per key.

source "$(dirname "$0")/common.sh"

PROBE="$TEST_DIR/tests/paste_probe.py"

# A burst must collapse into a single redraw. The probe feeds N bytes
# as one write and counts the cursor-home sequence that every
# editor_refresh() emits.
test_burst_single_repaint() {
    if ! command -v python3 &> /dev/null; then
        report_test "Paste burst collapses to one repaint" "PASS" "python3 not installed"
        return
    fi

    local test_file="paste_burst.txt"
    local burst=500
    printf 'first line\n' > "$test_file" || throw "Failed to create $test_file"

    local result
    result=$(python3 "$PROBE" "$EDITOR_BIN" "$test_file" "$burst" 2>/dev/null)
    local redraws
    redraws=$(echo "$result" | awk '{print $1}')

    if [ -z "$redraws" ]; then
        report_test "Paste burst collapses to one repaint" "FAIL" "probe produced no result"
        return
    fi

    if [ "$redraws" -le 5 ]; then
        report_test "Paste burst collapses to one repaint" "PASS"
    else
        report_test "Paste burst collapses to one repaint" "FAIL" \
            "$redraws repaints for $burst keys"
    fi

    rm -f "$test_file"
}

# The pasted bytes must all reach the buffer, not just the repaint count.
test_burst_content_intact() {
    if ! command -v expect &> /dev/null; then
        report_test "Paste burst content intact" "PASS" "expect not installed"
        return
    fi

    local test_file="paste_content.txt"
    local burst=200
    printf 'first line\n' > "$test_file" || throw "Failed to create $test_file"

    expect -c "
        set timeout 30
        spawn $EDITOR_BIN $test_file
        sleep 1               ;# let the editor finish its first draw
        send -- [string repeat x $burst]
        sleep 1
        send \"\x0F\"      ;# Ctrl-O to save
        sleep 1
        send \"\\r\"       ;# confirm the prefilled file name
        sleep 1
        send \"\x18\"      ;# Ctrl-X to quit
        expect eof
    " > /dev/null 2>&1

    local line_len
    line_len=$(head -1 "$test_file" | wc -c)
    # first line (10 chars + newline) plus the burst
    if [ "$line_len" -eq $((10 + burst + 1)) ]; then
        report_test "Paste burst content intact" "PASS"
    else
        report_test "Paste burst content intact" "FAIL" \
            "first line has $((line_len - 1)) chars, expected $((10 + burst))"
    fi

    rm -f "$test_file"
}

# Run tests
test_burst_single_repaint
test_burst_content_intact
