#!/usr/bin/env python3
"""Build and test the COW kernel in a disposable copy of Lab 5."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


LAB = Path(__file__).resolve().parent
PASS_MARKERS = (
    "cowtest: parent and child writes isolated",
    "cowtest: wait copyout isolated, last owner writable",
    "cowtest: read-only writes and copyout rejected",
    "cowtest: 32 live children share the 5 MiB region",
    "cowtest pass.",
    "init check memory pass.",
)


def run_demo():
    with tempfile.TemporaryDirectory(prefix="lab5-cow-") as directory:
        tree = Path(directory) / "code"
        shutil.copytree(
            LAB.parent,
            tree,
            ignore=shutil.ignore_patterns("cow_lab", "bin", "obj", ".qemu.out", ".gdb.in"),
        )
        for name in ("cow.c", "cow.h"):
            shutil.copyfile(LAB / name, tree / "kern" / "mm" / name)
        shutil.copyfile(LAB / "tests" / "cowtest.c", tree / "user" / "cowtest.c")
        subprocess.run(
            ["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", LAB / "integration.patch"],
            cwd=tree,
            check=True,
        )
        subprocess.run(["make", "build-cowtest"], cwd=tree, check=True)

        qemu = os.environ.get("QEMU", "qemu-system-riscv64")
        result = subprocess.run(
            ["make", "qemu", f"QEMU={qemu} -bios default"],
            cwd=tree,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=30,
        )
        print(result.stdout, end="")
        missing = [marker for marker in PASS_MARKERS if marker not in result.stdout]
        if result.returncode != 0 or "panic" in result.stdout or missing:
            raise RuntimeError(f"COW QEMU test failed. Missing output: {missing}")


if __name__ == "__main__":
    try:
        run_demo()
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
