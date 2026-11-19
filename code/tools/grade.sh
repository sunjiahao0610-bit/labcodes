#!/bin/sh

verbose=false
if [ 'x$1' = 'x-v' ]; then
    verbose=true
    out=/dev/stdout
    err=/dev/stderr
else
    out=/dev/null
    err=/dev/null
fi

if gmake --version > /dev/null 2>&1; then
    make=gmake
else
    make=make
fi

makeopts='--quiet --no-print-directory -j'

make_print() {
    echo `$make $makeopts print-$1`
}

awk='awk'
bc='bc'
date='date'
grep='grep'
sed='sed'

sym_table='obj/kernel.sym'
gdb=$(make_print GDB)
gdbport='1234'
gdb_in=$(make_print GRADE_GDB_IN)
qemu=$(make_print qemu)
qemu_out=$(make_print GRADE_QEMU_OUT)

if $qemu -nographic -help | grep -q '^-gdb'; then
    qemugdb=-gdb\ tcp::$gdbport
else
    qemugdb=-s\ -p\ $gdbport
fi

default_timeout=30
pts=0
part=0
part_pos=0
total=0
total_pos=0

update_score() {
    total=`expr $total + $part`
    total_pos=`expr $total_pos + $part_pos`
    part=0
    part_pos=0
}

get_time() {
    echo `$date +%s.%N 2> /dev/null`
}

show_final() {
    update_score
    printf 'Total Score: %s/%s\n' $total $total_pos
    if [ $total -lt $total_pos ]; then
        exit 1
    fi
}

show_time() {
    t1=$(get_time)
    time=`echo scale=1\;\ \($t1-$t0\)/1 | $sed 's/.N/.0/g' | $bc 2> /dev/null`
    printf '(%ss)\n' $time
}

show_build_tag() {
    printf '%-24s ' $1:
}

show_check_tag() {
    printf '  -%-40s  ' $1:
}

show_msg() {
    printf '%s\n' $1
    shift
    if [ $# -gt 0 ]; then
        printf '   %s\n' $@
        echo
    fi
}

pass() {
    show_msg OK $@
    part=`expr $part + $pts`
    part_pos=`expr $part_pos + $pts`
}

fail() {
    show_msg WRONG $@
    part_pos=`expr $part_pos + $pts`
}

run_qemu() {
    qemuextra=
    if [ -n "$brkfun" ]; then
        qemuextra=-S\ $qemugdb
    fi

    if [ -z $timeout ] || [ $timeout -le 0 ]; then
        timeout=$default_timeout
    fi

    t0=$(get_time)
    (
        ulimit -t $timeout
        exec $qemu -nographic $qemuopts -serial file:$qemu_out \
            -monitor null -no-reboot $qemuextra
    ) > $out 2> $err &
    pid=$!

    sleep 1

    if [ -n "$brkfun" ]; then
        brkaddr=`$grep ' '$brkfun\$ $sym_table | $sed -e's/ .*$//g'`
        brkaddr_phys=`echo $brkaddr | sed 's/^c0/00/g'`
        (
            echo target\ remote\ localhost:$gdbport
            echo break\ *0x$brkaddr
            if [ $brkaddr != $brkaddr_phys ]; then
                echo break\ *0x$brkaddr_phys
            fi
            echo continue
        ) > $gdb_in

        $gdb -batch -nx -x $gdb_in > /dev/null 2>&1
        kill $pid > /dev/null 2>&1
    else
        sleep 5
        kill $pid > /dev/null 2>&1
        wait $pid > /dev/null 2>&1
    fi
}

build_run() {
    show_build_tag $1
    shift
    $make $makeopts $@ 'DEFS+=-DDEBUG_GRADE' > $out 2> $err
    if [ $? -ne 0 ]; then
        printf '%s %s failed\n' $make $@
        exit 1
    fi
    run_qemu
    show_time
}

check_regexps() {
    okay=yes
    not=0
    reg=0
    error=
    for i do
        if [ $i = '!' ]; then
            not=1
        elif [ $i = '-' ]; then
            reg=1
        else
            if [ $reg -ne 0 ]; then
                echo $i | $grep -E -f - $qemu_out > /dev/null
            else
                echo $i | $grep -F -f - $qemu_out > /dev/null
            fi
            found=$(($? == 0))
            if [ $found -eq $not ]; then
                if [ $found -eq 0 ]; then
                    msg=missing\ $i
                else
                    msg=unexpected\ $i
                fi
                okay=no
                error=$msg
            fi
            not=0
            reg=0
        fi
    done
    if [ $okay = yes ]; then
        pass
    else
        fail $error
        if $verbose; then
            exit 1
        fi
    fi
}

check_result() {
    show_check_tag $1
    shift
    if [ ! -s $qemu_out ]; then
        sleep 4
    fi
    if [ ! -s $qemu_out ]; then
        fail > /dev/null
        echo no\ qemu\ output
    else
        check=$1
        shift
        $check $@
    fi
}

quick_check() {
    tag=$1
    shift
    check_result $tag check_regexps $@
}

osimg=$(make_print ucoreimg)
swapimg=$(make_print swapimg)
qemuopts='-machine virt -nographic -kernel bin/ucore.img'
brkfun=

$make $makeopts touch > /dev/null 2>&1
build_run 'Lab 9'

printf 'check Clock replacement: '
if $grep -F -f - $qemu_out <<'LAB9_CLOCK_PATTERN'
[Test 1] Clock Page Replacement: PASS (40/40)
LAB9_CLOCK_PATTERN
then
    part=40
    part_pos=40
    printf 'OK\n'
else
    part=0
    part_pos=40
    printf 'WRONG\n'
fi
update_score

printf 'check VMA mmap: '
if $grep -F -f - $qemu_out <<'LAB9_MMAP_PATTERN'
[Test 2] VMA mmap: PASS (30/30)
LAB9_MMAP_PATTERN
then
    part=30
    part_pos=30
    printf 'OK\n'
else
    part=0
    part_pos=30
    printf 'WRONG\n'
fi
update_score

printf 'check VMA munmap: '
if $grep -F -f - $qemu_out <<'LAB9_MUNMAP_PATTERN'
[Test 3] VMA munmap: PASS (30/30)
LAB9_MUNMAP_PATTERN
then
    part=30
    part_pos=30
    printf 'OK\n'
else
    part=0
    part_pos=30
    printf 'WRONG\n'
fi
update_score

show_final
