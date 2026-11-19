#!/bin/sh

verbose=false
if [ "x$1" = "x-v" ]; then
    verbose=true
    out=/dev/stdout
    err=/dev/stderr
else
    out=/dev/null
    err=/dev/null
fi

## make & makeopts
if gmake --version > /dev/null 2>&1; then
    make=gmake;
else
    make=make;
fi

makeopts="--quiet --no-print-directory -j"

make_print() {
    echo `$make $makeopts print-$1`
}

echo `$make`

## command tools
awk='awk'
bc='bc'
date='date'
grep='grep'
rm='rm -f'
sed='sed'

## symbol table
sym_table='obj/kernel.sym'

## gdb & gdbopts
gdb="$(make_print GDB)"
gdbport="1234"
gdb_in="$(make_print GRADE_GDB_IN)"

## qemu
qemu="$(make_print QEMU)"

qemu_out="$(make_print GRADE_QEMU_OUT)"

if $qemu -nographic -help | grep -q '^-gdb'; then
    qemugdb="-gdb tcp::$gdbport"
else
    qemugdb="-s -p $gdbport"
fi

## score
part=""
total=0
total_pos=0

get_time() {
    if [ "$date" = "date" ]; then
        date +%s
    else
        echo 0
    fi
}

show_part_score() {
    if [ -z "$part" ]; then
        score_msg=""
    else
        score_msg="[$part/$part_pos]"
    fi
    echo
    update_score
}

update_score() {
    total=`expr $total + $part`
    total_pos=`expr $total_pos + $part_pos`
    part=""
    part_pos=0
}

show_final() {
    update_score
    echo "Total Score: $total/$total_pos"
    if [ $total -lt $total_pos ]; then
        exit 1
    fi
}

show_time() {
    t1=$(get_time)
    time=`echo "scale=1; ($t1-$t0)/1" | $sed 's/.N/.0/g' | $bc 2> /dev/null`
    echo "(${time}s)"
}

show_build_tag() {
    echo "$1:" | $awk '{printf "%-24s ", $0}'
}

show_check_tag() {
    echo "$1:" | $awk '{printf "  -%-40s  ", $0}'
}

show_msg() {
    echo $1
    shift
    if [ $# -gt 0 ]; then
        echo -e "$@" | awk '{printf "   %s\n", $0}'
        echo
    fi
}

pass() {
    show_msg OK "$@"
    part=`expr $part + $pts`
    part_pos=`expr $part_pos + $pts`
}

fail() {
    show_msg WRONG "$@"
    part_pos=`expr $part_pos + $pts`
}

## run qemu and collect the serial output
default_timeout=90
gdb_timeout=60
brkfun=readline

# Bound every external command by wall-clock time. `ulimit -t` only limits CPU
# time, and a kernel that spins in an exception handler or that waits for
# console input burns almost no CPU, so it would never be stopped by that.
if command -v timeout > /dev/null 2>&1; then
    TMO="timeout"
else
    TMO=""
fi

run_qemu() {
    # Run qemu with the serial output redirected to $qemu_out. When $brkfun is
    # set, gdb continues until that kernel function is reached; the injected
    # command script makes sh enter it, so the captured output holds everything
    # the graded user programs printed.
    #
    # Both qemu and gdb are started in the background under a wall-clock
    # timeout and then waited for. If the breakpoint never fires because the
    # student kernel is broken, gdb would otherwise wait forever and grading
    # would hang.
    qemuextra=
    if [ -n "$brkfun" ]; then
        qemuextra="-S $qemugdb"
    fi

    # A stale output file from an earlier run would be mistaken for this run's
    # output, so always start from an empty one.
    $rm -f $qemu_out

    t0=$(get_time)

    (
        $TMO -s KILL $default_timeout \
            $qemu -nographic $qemuopts -serial file:$qemu_out -monitor null -no-reboot $qemuextra
    ) > $out 2> $err &
    qpid=$!

    # wait for QEMU to start and for gdb to attach
    sleep 1

    if [ -n "$brkfun" ]; then
        brkaddr=`$grep " $brkfun\$" $sym_table | $sed -e's/ .*$//g'`
        brkaddr_phys=`echo $brkaddr | sed "s/^c0/00/g"`
        (
            echo "target remote localhost:$gdbport"
            echo "break *0x$brkaddr"
            if [ "$brkaddr" != "$brkaddr_phys" ]; then
                echo "break *0x$brkaddr_phys"
            fi
            echo "continue"
        ) > $gdb_in

        (
            $TMO -s KILL $gdb_timeout $gdb -batch -nx -x $gdb_in
        ) > $out 2> $err
    else
        # no breakpoint: let the kernel run for the whole budget
        sleep $default_timeout
    fi

    # make sure that QEMU is dead
    kill $qpid > /dev/null 2>&1
    wait $qpid > /dev/null 2>&1
}

check_result() {
    # usage: check_result <tag> <check> <check args...>
    show_check_tag "$1"
    shift

    if [ ! -s $qemu_out ]; then
        sleep 4
    fi

    if [ ! -s $qemu_out ]; then
        fail > /dev/null
        echo 'no $qemu_out'
    else
        check=$1
        shift
        $check "$@"
    fi
}

check_regexps() {
    okay=yes
    not=0
    reg=0
    error=
    for i do
        if [ "x$i" = "x!" ]; then
            not=1
        elif [ "x$i" = "x-" ]; then
            reg=1
        else
            if [ $reg -ne 0 ]; then
                $grep '-E' "^$i\$" $qemu_out > /dev/null
            else
                $grep '-F' "$i" $qemu_out > /dev/null
            fi
            found=$(($? == 0))
            if [ $found -eq $not ]; then
                if [ $found -eq 0 ]; then
                    msg="!! error: missing '$i'"
                else
                    msg="!! error: got unexpected line '$i'"
                fi
                okay=no
                if [ -z "$error" ]; then
                    error="$msg"
                else
                    error="$error\n$msg"
                fi
            fi
            not=0
            reg=0
        fi
    done
    if [ "$okay" = "yes" ]; then
        pass
    else
        fail "$error"
        if $verbose; then
            exit 1
        fi
    fi
}

## check_no_faults - fail when the run produced any kernel fault message.
## A clean run of every user program must not print one.
check_no_faults() {
    bad=no
    first=
    for pat in \
        'Store/AMO page fault' \
        'Load page fault' \
        'Instruction page fault' \
        'Illegal instruction' \
        'Instruction address misaligned' \
        'Load address misaligned' \
        'assertion failed'
    do
        if $grep -F "$pat" $qemu_out > /dev/null; then
            bad=yes
            if [ -z "$first" ]; then
                first="$pat"
            fi
        fi
    done
    if [ "$bad" = "yes" ]; then
        fail "kernel fault: $first"
    else
        pass
    fi
}

quick_check() {
    # usage: quick_check <tag> checkargs ...
    tag="$1"
    shift
    check_result "$tag" check_regexps "$@"
}

## kernel image
osimg=$(make_print ucoreimg)
swapimg=$(make_print swapimg)

## set default qemu-options
qemuopts="-machine virt -nographic -kernel bin/ucore.img"

## The injected files change contents without touching the target timestamps,
## so drop the previous artifacts to force a full rebuild of the injected
## program. `make clean` is not used here because it also removes disk0, which
## is the SFS source directory holding the injected canary and command script.
$rm -rf obj bin
$make $makeopts > $out 2> $err

if [ ! -s bin/ucore.img ] || [ ! -s bin/sfs.img ]; then
    echo "build failed: bin/ucore.img or bin/sfs.img is missing"
    exit 1
fi

## run the injected kernel once; every check below looks at the same output
run_qemu

## check now!!

pts=10
quick_check 'sh execve'                          \
    'vfs: mount disk0.'                                 \
    'kernel_execve: pid = 2, name = "sh".'

pts=10
quick_check 'user sh'                            \
    'user sh is running!!!'

pts=25
quick_check 'SFS read path'                      \
    '[Test 1] SFS Read: PASS'

pts=15
quick_check 'load_icode / sh runs hello'         \
    'Hello world!!.'                                    \
    'hello pass.'

pts=20
quick_check 'fork / wait / file table'           \
    'exit pass.'                                        \
    'forktest pass.'

pts=10
quick_check 'yield / scheduler'                  \
    'yield pass.'

pts=10
check_result 'no kernel failures' check_no_faults

## print final-score
show_final
