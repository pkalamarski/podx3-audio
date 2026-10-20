// Real-time scheduling for selected threads of this process.
//
// podx3_make_all_threads_realtime() would also pin XPC/dispatch workers, and it
// has to be re-run after every reopen anyway: libusb's darwin backend starts its
// IOKit completion thread ("org.libusb.device-hotplug") on the first
// libusb_init and stops it on the last libusb_exit. So the helper picks threads
// by name instead. Same policy numbers as podx3_usb.c (1 ms period, 0.2 ms
// computation).
#pragma once

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <string.h>

static inline int podx3_rt_policy(thread_act_t th) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ns_to_abs = (double)tb.denom / tb.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)(1000000 * ns_to_abs),
        .computation = (uint32_t)(200000 * ns_to_abs),
        .constraint = (uint32_t)(1000000 * ns_to_abs),
        .preemptible = 1,
    };
    return thread_policy_set(th, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
                             THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS ? 0 : -1;
}

// Applies the policy to every thread whose pthread name equals name.
// Returns the number of threads changed, or -1 on failure.
static inline int podx3_rt_named_threads(const char *name) {
    thread_act_array_t threads;
    mach_msg_type_number_t n;
    if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS) return -1;
    int done = 0, fails = 0;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        thread_extended_info_data_t info;
        mach_msg_type_number_t cnt = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, (thread_info_t)&info, &cnt) == KERN_SUCCESS &&
            !strncmp(info.pth_name, name, sizeof info.pth_name)) {
            if (podx3_rt_policy(threads[i])) fails++;
            else done++;
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, n * sizeof(thread_act_t));
    return fails ? -1 : done;
}
