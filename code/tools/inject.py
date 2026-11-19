#!/usr/bin/env python3
"""Inject Lab8 grading tests into a temporary student-tree copy.

The injector replaces the body of user_main() with test code, places a canary
binary file, and queues console commands. The student's original sources are
never modified.
"""

import os
import sys


def die(message):
    print("[lab8 inject] ERROR: " + message)
    raise SystemExit(1)


if len(sys.argv) != 2:
    die("Usage: inject.py <temporary grading copy>")

root = os.path.abspath(sys.argv[1])
tools = os.path.dirname(os.path.abspath(__file__))

def read_text(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def read_binary(path):
    with open(path, "rb") as f:
        return f.read()


def write_binary(path, data):
    with open(path, "wb") as f:
        f.write(data)


def function_body_span(text, anchor):
    """Return (body_start, body_end) for the function whose signature contains anchor.

    body_start is the index just after the opening brace of the function and
    body_end is the index of its matching closing brace. Braces inside strings,
    character literals and comments are skipped.
    """
    sig = text.find(anchor)
    if sig < 0:
        die("Cannot find the anchor '{}' in file".format(anchor))
    open_brace = text.find("{", sig + len(anchor))
    if open_brace < 0:
        die("Cannot find the body of '{}'".format(anchor))

    depth = 0
    i = open_brace
    n = len(text)
    while i < n:
        c = text[i]
        # line comment
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            if i < 0:
                die("Unterminated line comment in '{}'".format(anchor))
            continue
        # block comment
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            if end < 0:
                die("Unterminated block comment in '{}'".format(anchor))
            i = end + 2
            continue
        # string or character literal
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return open_brace + 1, i
        i += 1
    die("Unbalanced braces after the body of '{}'".format(anchor))


proc_path = os.path.join(root, "kern", "process", "proc.c")
stdin_path = os.path.join(root, "kern", "fs", "devs", "dev_stdin.c")
body_src = os.path.join(tools, "test_lab8_user.h")
stdin_src = os.path.join(tools, "test_lab8_stdin.h")
canary_src = os.path.join(tools, "canary.bin")
canary_dst = os.path.join(root, "disk0", "canary")

# Pre-condition: verify the skeleton is clean
proc_text = read_text(proc_path)
if "[Test 1] SFS Read" in proc_text:
    die("proc.c is already injected")
stdin_text = read_text(stdin_path)
if "lab8_script" in stdin_text:
    die("dev_stdin.c is already injected")

# Read test body and stdin body
if not os.path.exists(body_src):
    die("tools/test_lab8_user.h not found")
body = read_text(body_src).strip()

if not os.path.exists(stdin_src):
    die("tools/test_lab8_stdin.h not found")
stdin_body = read_text(stdin_src).strip()

# Replace the body of user_main()
start, end = function_body_span(proc_text, "user_main(void *arg)")
proc_text = proc_text[:start] + "\n" + body + "\n" + proc_text[end:]
write_text(proc_path, proc_text)

# Place the canary file
if not os.path.exists(canary_src):
    die("tools/canary.bin not found")
canary = read_binary(canary_src)
disk0 = os.path.join(root, "disk0")
if not os.path.isdir(disk0):
    os.makedirs(disk0)
write_binary(canary_dst, canary)

# Queue the graded commands on the console
stdin_sig = stdin_text.find("dev_stdin_read(char *buf, size_t len)")
if stdin_sig < 0:
    die("Cannot find dev_stdin_read anchor in dev_stdin.c")
open_brace = stdin_text.find("{", stdin_sig + len("dev_stdin_read(char *buf, size_t len)"))
if open_brace < 0:
    die("Cannot find the body of dev_stdin_read")
stdin_text = stdin_text[:open_brace + 1] + "\n" + stdin_body + "\n" + stdin_text[open_brace + 1:]
write_text(stdin_path, stdin_text)

print("lab8 injection complete")

