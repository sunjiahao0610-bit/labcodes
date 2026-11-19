#include <defs.h>
#include <assert.h>
#include <proc.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <default_sched.h>

static void lab6_pass(const char *name)
{
    cprintf("[Test %s] PASS\n", name);
}

void test_lab6_rr_scheduler(void)
{
    extern struct sched_class default_sched_class;
    
    // Verify scheduler structure
    assert(default_sched_class.name != NULL);
    assert(strcmp(default_sched_class.name, "RR_scheduler") == 0);
    assert(default_sched_class.init != NULL);
    assert(default_sched_class.enqueue != NULL);
    assert(default_sched_class.dequeue != NULL);
    assert(default_sched_class.pick_next != NULL);
    assert(default_sched_class.proc_tick != NULL);
    
    // Test basic functionality with a mock run queue
    struct run_queue test_rq;
    memset(&test_rq, 0, sizeof(test_rq));
    default_sched_class.init(&test_rq);
    assert(test_rq.proc_num == 0);
    assert(test_rq.max_time_slice == MAX_TIME_SLICE);
    
    // Test with a mock process
    struct proc_struct mock_proc;
    memset(&mock_proc, 0, sizeof(mock_proc));
    list_init(&(mock_proc.run_link));
    mock_proc.time_slice = 0;
    mock_proc.pid = 100;
    
    default_sched_class.enqueue(&test_rq, &mock_proc);
    assert(test_rq.proc_num == 1);
    assert(mock_proc.time_slice == MAX_TIME_SLICE);
    
    struct proc_struct *p = default_sched_class.pick_next(&test_rq);
    assert(p != NULL && p->pid == 100);
    
    mock_proc.time_slice = 1;
    mock_proc.need_resched = 0;
    default_sched_class.proc_tick(&test_rq, &mock_proc);
    assert(mock_proc.time_slice == 0);
    assert(mock_proc.need_resched == 1);
    
    default_sched_class.dequeue(&test_rq, &mock_proc);
    assert(test_rq.proc_num == 0);
    
    lab6_pass("RR Scheduler");
}

void test_lab6_stride_scheduler(void)
{
    extern struct sched_class stride_sched_class;
    
    // Verify scheduler structure
    assert(stride_sched_class.name != NULL);
    assert(strcmp(stride_sched_class.name, "stride_scheduler") == 0);
    assert(stride_sched_class.init != NULL);
    assert(stride_sched_class.enqueue != NULL);
    assert(stride_sched_class.dequeue != NULL);
    assert(stride_sched_class.pick_next != NULL);
    assert(stride_sched_class.proc_tick != NULL);
    
    // Test basic functionality with a mock run queue
    struct run_queue test_rq;
    memset(&test_rq, 0, sizeof(test_rq));
    stride_sched_class.init(&test_rq);
    assert(test_rq.proc_num == 0);
    assert(test_rq.lab6_run_pool == NULL);
    
    // Test with mock processes
    struct proc_struct mock_proc1, mock_proc2;
    
    memset(&mock_proc1, 0, sizeof(mock_proc1));
    skew_heap_init(&(mock_proc1.lab6_run_pool));
    mock_proc1.pid = 101;
    mock_proc1.lab6_priority = 1;
    mock_proc1.lab6_stride = 0;
    
    memset(&mock_proc2, 0, sizeof(mock_proc2));
    skew_heap_init(&(mock_proc2.lab6_run_pool));
    mock_proc2.pid = 102;
    mock_proc2.lab6_priority = 2;
    mock_proc2.lab6_stride = 100;
    
    stride_sched_class.enqueue(&test_rq, &mock_proc1);
    assert(test_rq.proc_num == 1);
    assert(test_rq.lab6_run_pool != NULL);
    
    stride_sched_class.enqueue(&test_rq, &mock_proc2);
    assert(test_rq.proc_num == 2);
    
    // Test pick_next selects process with smallest stride
    struct proc_struct *p = stride_sched_class.pick_next(&test_rq);
    assert(p != NULL && p->pid == 101);
    assert(mock_proc1.lab6_stride > 0);
    
    // Test proc_tick
    mock_proc1.time_slice = 1;
    mock_proc1.need_resched = 0;
    stride_sched_class.proc_tick(&test_rq, &mock_proc1);
    assert(mock_proc1.time_slice == 0);
    assert(mock_proc1.need_resched == 1);
    
    // Test dequeue
    stride_sched_class.dequeue(&test_rq, &mock_proc1);
    assert(test_rq.proc_num == 1);
    
    stride_sched_class.dequeue(&test_rq, &mock_proc2);
    assert(test_rq.proc_num == 0);
    assert(test_rq.lab6_run_pool == NULL);
    
    lab6_pass("Stride Scheduler");
}

