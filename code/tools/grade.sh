#!/bin/sh

set -u

make_cmd=make
if gmake --version >/dev/null 2>&1; then
    make_cmd=gmake
fi

output=.qemu.out
score=0

echo "Building Lab7 tests..."
if ! "$make_cmd" --quiet --no-print-directory clean; then
    echo "  - build: FAIL (clean)"
    echo "Total Score: 0/100"
    exit 1
fi

if ! "$make_cmd" --quiet --no-print-directory "DEFS+=-DDEBUG_GRADE"; then
    echo "  - build: FAIL"
    echo "Total Score: 0/100"
    exit 1
fi

echo "Running Lab7 tests..."
rm -f "$output"
timeout 40s qemu-system-riscv64 \
    -machine virt -nographic \
    -kernel bin/ucore.img \
    -serial file:"$output" -monitor none -no-reboot >/dev/null 2>&1 || true

if grep -F "[Test 1] Condition Variable: PASS" "$output" >/dev/null 2>&1; then
    echo "  - condition variable: PASS (50/50)"
    score=$((score + 50))
else
    echo "  - condition variable: FAIL (0/50)"
fi

if grep -F "[Test 2] Philosopher Monitor: PASS" "$output" >/dev/null 2>&1; then
    echo "  - philosopher monitor: PASS (50/50)"
    score=$((score + 50))
else
    echo "  - philosopher monitor: FAIL (0/50)"
fi

echo "Total Score: $score/100"
[ "$score" -eq 100 ]
