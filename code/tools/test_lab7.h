#ifndef __KERN_TEST_TEST_LAB7_H__
#define __KERN_TEST_TEST_LAB7_H__

#include <defs.h>

typedef void (*test_lab7_phi_fn_t)(int32_t);

void test_lab7_condvar(void);
void test_lab7_philosopher(test_lab7_phi_fn_t test_fn,
                           test_lab7_phi_fn_t take_fn,
                           test_lab7_phi_fn_t put_fn);

#endif /* !__KERN_TEST_TEST_LAB7_H__ */
