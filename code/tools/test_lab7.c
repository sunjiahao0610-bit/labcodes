#include <defs.h>
#include <stdio.h>
#include <proc.h>
#include <sem.h>
#include <monitor.h>
#include <test_lab7.h>

#define TEST_LAB7_N             5
#define TEST_LAB7_THINKING      0
#define TEST_LAB7_HUNGRY        1
#define TEST_LAB7_EATING        2
#define TEST_LAB7_LEFT(i)       (((i) + TEST_LAB7_N - 1) % TEST_LAB7_N)
#define TEST_LAB7_RIGHT(i)      (((i) + 1) % TEST_LAB7_N)

extern monitor_t mt;
extern int state_condvar[];
extern int philosopher_using_condvar(void *arg);

static monitor_t test_cv_mt;
static semaphore_t test_cv_ready;
static volatile int test_cv_awake;
static volatile int test_cv_waiter_error;
static monitor_t test_handoff_mt;
static semaphore_t test_handoff_started, test_handoff_done;
static volatile int test_handoff_error;
static volatile int test_phi_running;
static volatile int test_phi_bad;

static int
test_cv_waiter(void *arg)
{
    (void)arg;

    down(&test_cv_mt.mutex);
    up(&test_cv_ready);
    cond_wait(&test_cv_mt.cv[0]);

    /* A correct Hoare monitor wakes this thread as the monitor owner. */
    if (test_cv_mt.mutex.value != 0 || test_cv_mt.next_count != 1) {
        test_cv_waiter_error = 1;
    }
    test_cv_awake++;

    /* Leave the monitor and let the blocked signaler resume. */
    if (test_cv_mt.next_count > 0) {
        up(&test_cv_mt.next);
    } else {
        up(&test_cv_mt.mutex);
    }
    return 0;
}

static int
test_cv_handoff_waiter(void *arg)
{
    (void)arg;

    down(&test_handoff_mt.mutex);
    up(&test_handoff_started);
    cond_wait(&test_handoff_mt.cv[0]);
    cond_wait(&test_handoff_mt.cv[1]);

    if (test_handoff_mt.mutex.value != 0 ||
        test_handoff_mt.next_count != 1) {
        test_handoff_error = 1;
    }
    up(&test_handoff_mt.next);
    up(&test_handoff_done);
    return 0;
}

static int
test_cv_handoff(void)
{
    int pid;

    test_handoff_error = 0;
    sem_init(&test_handoff_started, 0);
    sem_init(&test_handoff_done, 0);
    monitor_init(&test_handoff_mt, 2);

    pid = kernel_thread(test_cv_handoff_waiter, NULL, 0);
    if (pid <= 0) {
        monitor_free(&test_handoff_mt, 2);
        return 0;
    }

    down(&test_handoff_started);
    down(&test_handoff_mt.mutex);
    cond_signal(&test_handoff_mt.cv[0]);

    if (test_handoff_mt.cv[1].count != 1 ||
        test_handoff_mt.next_count != 0 ||
        test_handoff_mt.mutex.value != 0) {
        test_handoff_error = 1;
    }

    cond_signal(&test_handoff_mt.cv[1]);
    up(&test_handoff_mt.mutex);
    down(&test_handoff_done);
    do_wait(pid, NULL);
    monitor_free(&test_handoff_mt, 2);
    return test_handoff_error == 0;
}

void
test_lab7_condvar(void)
{
    int ok = 1;
    int old_count, old_next_count, old_mutex_value;
    int old_next_value, old_cv_value, old_queue_empty;
    int pid1, pid2;

    test_cv_awake = 0;
    test_cv_waiter_error = 0;
    sem_init(&test_cv_ready, 0);
    monitor_init(&test_cv_mt, 1);

    /* Signaling an empty condition variable must be a no-op. */
    down(&test_cv_mt.mutex);
    old_count = test_cv_mt.cv[0].count;
    old_next_count = test_cv_mt.next_count;
    old_mutex_value = test_cv_mt.mutex.value;
    old_next_value = test_cv_mt.next.value;
    old_cv_value = test_cv_mt.cv[0].sem.value;
    old_queue_empty = wait_queue_empty(&test_cv_mt.cv[0].sem.wait_queue);

    cond_signal(&test_cv_mt.cv[0]);

    if (test_cv_mt.cv[0].count != old_count ||
        test_cv_mt.next_count != old_next_count ||
        test_cv_mt.mutex.value != old_mutex_value ||
        test_cv_mt.next.value != old_next_value ||
        test_cv_mt.cv[0].sem.value != old_cv_value ||
        wait_queue_empty(&test_cv_mt.cv[0].sem.wait_queue) != old_queue_empty) {
        ok = 0;
    }
    up(&test_cv_mt.mutex);

    if (!ok) {
        monitor_free(&test_cv_mt, 1);
        cprintf("[Test 1] Condition Variable: FAIL (empty signal changed state)\n");
        return;
    }

    pid1 = kernel_thread(test_cv_waiter, NULL, 0);
    pid2 = kernel_thread(test_cv_waiter, NULL, 0);
    if (pid1 <= 0 || pid2 <= 0) {
        monitor_free(&test_cv_mt, 1);
        cprintf("[Test 1] Condition Variable: FAIL (create waiter)\n");
        return;
    }

    /* Both waiters have entered cond_wait and released the monitor. */
    down(&test_cv_ready);
    down(&test_cv_ready);
    down(&test_cv_mt.mutex);
    if (test_cv_mt.cv[0].count != 2) {
        ok = 0;
    }

    /* One signal must wake exactly one waiter and transfer monitor ownership. */
    cond_signal(&test_cv_mt.cv[0]);
    if (test_cv_awake != 1 || test_cv_mt.cv[0].count != 1 ||
        test_cv_mt.next_count != 0) {
        ok = 0;
    }

    /* The second signal wakes the remaining waiter. */
    cond_signal(&test_cv_mt.cv[0]);
    if (test_cv_awake != 2 || test_cv_mt.cv[0].count != 0 ||
        test_cv_mt.next_count != 0) {
        ok = 0;
    }
    up(&test_cv_mt.mutex);

    do_wait(pid1, NULL);
    do_wait(pid2, NULL);
    if (test_cv_waiter_error) {
        ok = 0;
    }
    if (!test_cv_handoff()) {
        ok = 0;
    }
    monitor_free(&test_cv_mt, 1);

    if (ok) {
        cprintf("[Test 1] Condition Variable: PASS (50/50)\n");
    } else {
        cprintf("[Test 1] Condition Variable: FAIL (wake/count/monitor state)\n");
    }
}

static test_lab7_phi_fn_t test_phi_test_fn;
static test_lab7_phi_fn_t test_phi_take_fn;
static test_lab7_phi_fn_t test_phi_put_fn;
static semaphore_t test_phi_started, test_phi_finished;

static void
test_phi_set_all(int state)
{
    int i;
    for (i = 0; i < TEST_LAB7_N; i++) {
        state_condvar[i] = state;
    }
}

static int
test_phi_adjacent_eating(void)
{
    int i;
    for (i = 0; i < TEST_LAB7_N; i++) {
        if (state_condvar[i] == TEST_LAB7_EATING &&
            state_condvar[TEST_LAB7_RIGHT(i)] == TEST_LAB7_EATING) {
            return 1;
        }
    }
    return 0;
}

static int
test_phi_stress_thread(void *arg)
{
    philosopher_using_condvar(arg);
    test_phi_running--;
    return 0;
}

static int
test_phi_observer(void *arg)
{
    (void)arg;

    while (test_phi_running > 0) {
        if (test_phi_adjacent_eating()) {
            test_phi_bad = 1;
        }
        do_sleep(1);
    }
    return 0;
}

static int
test_phi_stress(void)
{
    int pids[TEST_LAB7_N];
    int observer_pid;
    int i, ok = 1;

    test_phi_running = TEST_LAB7_N;
    test_phi_bad = 0;
    test_phi_set_all(TEST_LAB7_THINKING);

    observer_pid = kernel_thread(test_phi_observer, NULL, 0);
    if (observer_pid <= 0) {
        return 0;
    }

    for (i = 0; i < TEST_LAB7_N; i++) {
        pids[i] = kernel_thread(test_phi_stress_thread, (void *)i, 0);
        if (pids[i] <= 0) {
            test_phi_running = 0;
            ok = 0;
            break;
        }
    }

    for (i = 0; i < TEST_LAB7_N && pids[i] > 0; i++) {
        do_wait(pids[i], NULL);
    }
    do_wait(observer_pid, NULL);

    if (test_phi_bad || test_phi_adjacent_eating()) {
        ok = 0;
    }
    return ok;
}

static int
test_phi_blocked_take(void *arg)
{
    (void)arg;
    up(&test_phi_started);
    test_phi_take_fn(0);
    up(&test_phi_finished);
    return 0;
}

static int
test_phi_blocked_by(int blocker)
{
    int ok = 1;
    int pid;

    test_phi_set_all(TEST_LAB7_THINKING);
    state_condvar[blocker] = TEST_LAB7_EATING;
    sem_init(&test_phi_started, 0);
    sem_init(&test_phi_finished, 0);
    pid = kernel_thread(test_phi_blocked_take, NULL, 0);
    if (pid <= 0) {
        return 0;
    }

    down(&test_phi_started);
    do_sleep(2);
    if (state_condvar[0] != TEST_LAB7_HUNGRY || mt.cv[0].count != 1) {
        ok = 0;
    }

    test_phi_put_fn(blocker);
    down(&test_phi_finished);
    do_wait(pid, NULL);
    if (state_condvar[0] != TEST_LAB7_EATING || test_phi_adjacent_eating()) {
        ok = 0;
    }
    return ok;
}

void
test_lab7_philosopher(test_lab7_phi_fn_t test_fn,
                      test_lab7_phi_fn_t take_fn,
                      test_lab7_phi_fn_t put_fn)
{
    int ok = 1;

    test_phi_test_fn = test_fn;
    test_phi_take_fn = take_fn;
    test_phi_put_fn = put_fn;
    monitor_init(&mt, TEST_LAB7_N);
    test_phi_set_all(TEST_LAB7_THINKING);

    /* phi_test_condvar must only promote HUNGRY when both forks are free. */
    down(&mt.mutex);
    state_condvar[0] = TEST_LAB7_HUNGRY;
    state_condvar[1] = TEST_LAB7_EATING;
    test_phi_test_fn(0);
    if (state_condvar[0] != TEST_LAB7_HUNGRY) {
        ok = 0;
    }

    state_condvar[0] = TEST_LAB7_THINKING;
    state_condvar[1] = TEST_LAB7_THINKING;
    test_phi_test_fn(0);
    if (state_condvar[0] != TEST_LAB7_THINKING) {
        ok = 0;
    }

    state_condvar[0] = TEST_LAB7_HUNGRY;
    test_phi_test_fn(0);
    if (state_condvar[0] != TEST_LAB7_EATING) {
        ok = 0;
    }
    up(&mt.mutex);

    /* The uncontended take/put path must return in EATING/THINKING states. */
    test_phi_set_all(TEST_LAB7_THINKING);
    test_phi_take_fn(0);
    if (state_condvar[0] != TEST_LAB7_EATING || test_phi_adjacent_eating()) {
        ok = 0;
    }
    test_phi_put_fn(0);
    if (state_condvar[0] != TEST_LAB7_THINKING || test_phi_adjacent_eating()) {
        ok = 0;
    }

    /* Exercise both neighbour checks in phi_put_forks_condvar(). */
    if (!test_phi_blocked_by(TEST_LAB7_LEFT(0)) ||
        !test_phi_blocked_by(TEST_LAB7_RIGHT(0))) {
        ok = 0;
    }

    test_phi_set_all(TEST_LAB7_THINKING);
    state_condvar[0] = TEST_LAB7_EATING;
    state_condvar[TEST_LAB7_LEFT(0)] = TEST_LAB7_HUNGRY;
    state_condvar[TEST_LAB7_RIGHT(0)] = TEST_LAB7_HUNGRY;
    test_phi_put_fn(0);
    if (state_condvar[TEST_LAB7_LEFT(0)] != TEST_LAB7_EATING ||
        state_condvar[TEST_LAB7_RIGHT(0)] != TEST_LAB7_EATING ||
        test_phi_adjacent_eating()) {
        ok = 0;
    }

    if (!test_phi_stress()) {
        ok = 0;
    }

    test_phi_set_all(TEST_LAB7_THINKING);
    monitor_free(&mt, TEST_LAB7_N);

    if (ok) {
        cprintf("[Test 2] Philosopher Monitor: PASS (50/50)\n");
    } else {
        cprintf("[Test 2] Philosopher Monitor: FAIL (state/wait/signal)\n");
    }
}
