#include "syscall.h"

#include <Timer.h>
#include <Objects/Process.h>

#include <Logging.h>

struct ModuleQuery {
    char name[256];
};

struct DeviceQuery {
    char name[256];
};

SYSCALL long le_log(le_str_t umsg) {
    String msg = get_user_string_or_fault(umsg, 0xffffffff);
    Process* process = Process::current(); 

    Log::Info("[%s] %s", process->name, msg.c_str());
    return 0;
}

SYSCALL long le_boot_timer() {
    return Timer::UsecondsSinceBoot();
}

SYSCALL long le_handle_close(le_handle_t handle) {
    Process* process = Process::current();

    return process->handle_destroy(handle);
}

SYSCALL long le_handle_dup(le_handle_t oldHandle, UserPointer<le_handle_t> _newHandle, int flags) {
    le_handle_t newHandle;
    if(_newHandle.get(newHandle)) {
        return EFAULT;
    }

    if(oldHandle == newHandle) {
        return EINVAL;
    }

    if(flags & (~O_CLOEXEC)) {
        Log::Warning("le_handle_dup: invalid value %x in flags", flags);
        return EINVAL;
    }

    Process* process = Process::current();

    Handle h = process->get_handle(oldHandle);
    if(!h.IsValid()) {
        return EBADF;
    }

    if(flags & O_CLOEXEC) {
        h.closeOnExec = true;
    }

    if(newHandle >= 0) {
        // Closing and resuing newHandle needs to be done atomically
        return process->handle_replace(newHandle, h);
    } else {
        le_handle_t id = process->allocate_handle(h.ko, h.closeOnExec);
        if(_newHandle.store(id)) {
            process->handle_destroy(id);
            return EFAULT;
        }

        return 0; 
    }
}

SYSCALL long le_futex_wait(UserPointer<int> futex, int expected, const struct timespec* time) {
    return 0; //ENOSYS;
}

SYSCALL long le_futex_wake(UserPointer<int> futex) {
    return 0; //ENOSYS;
}

SYSCALL long le_set_user_tcb(uintptr_t value) {
    Thread* thread = Thread::current();

    asm("cli");
    thread->fsBase = value;

    UpdateUserTCB(thread->fsBase);
    asm("sti");

    return 0;
}

SYSCALL long le_nanosleep(UserPointer<long> nanos) {
    long us;
    if(nanos.get(us)) {
        return EFAULT;
    }

    if(us < 0) {
        return EINVAL;
    }

    us /= 1000;
    
    long targetTime = Timer::UsecondsSinceBoot() + us;

    Thread::current()->sleep(us);

    targetTime -= Timer::UsecondsSinceBoot();

    if(Thread::current()->has_pending_signals() && targetTime > 0) {
        // Don't worry about EFAULT,
        // just return EINTR
        nanos.store(targetTime * 1000);

        return EINTR;
    }

    return 0;
}

SYSCALL long le_load_module(le_str_t path) {
    return ENOSYS;
}

SYSCALL long le_unload_module(le_str_t name) {
    return ENOSYS;
}

SYSCALL long le_query_modules() {
    return ENOSYS;
}

SYSCALL long le_query_devices() {
    return ENOSYS;
}
