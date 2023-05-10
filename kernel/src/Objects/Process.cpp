#include <Objects/Process.h>

#include <ABI.h>
#include <APIC.h>
#include <Assert.h>
#include <CPU.h>
#include <ELF.h>
#include <IDT.h>
#include <Panic.h>
#include <SMP.h>
#include <Scheduler.h>
#include <String.h>
#include <hiraku.h>

#include <abi/peb.h>

extern uint8_t signalTrampolineStart[];
extern uint8_t signalTrampolineEnd[];

extern uint8_t _user_shared_data[];
extern uint8_t _hiraku[];
extern uint8_t _hiraku_end[];
extern uint8_t _user_shared_data_end[];

// the user_shared_data section is aligned to page size.
class UserSharedData : public VMObject {
public:
    UserSharedData()
        : VMObject(PAGE_COUNT_4K(_user_shared_data_end - _user_shared_data) << PAGE_SHIFT_4K, false, true) {
    }

    void MapAllocatedBlocks(uintptr_t base, PageMap* pMap) {
        Memory::MapVirtualMemory4K(((uintptr_t)_user_shared_data - KERNEL_VIRTUAL_BASE), base, size >> PAGE_SHIFT_4K,
                                   PAGE_USER | PAGE_PRESENT, pMap);
    }

    [[noreturn]] VMObject* Clone() {
        assert(!"user_shared_data VMO cannot be cloned!");
    }
};
lock_t userSharedDataLock = 0;
FancyRefPtr<UserSharedData>* userSharedDataVMO = nullptr;

void IdleProcess();

FancyRefPtr<Process> Process::create_idle_process(const char* name) {
    FancyRefPtr<Process> proc = new Process(Scheduler::GetNextPID(), name, "/", nullptr);

    proc->m_mainThread->registers.rip = reinterpret_cast<uintptr_t>(IdleProcess);
    proc->m_mainThread->timeSlice = 0;
    proc->m_mainThread->timeSliceDefault = 0;

    proc->m_mainThread->registers.rsp = reinterpret_cast<uintptr_t>(proc->m_mainThread->kernelStack);
    proc->m_mainThread->registers.rbp = reinterpret_cast<uintptr_t>(proc->m_mainThread->kernelStack);

    proc->m_isIdleProcess = true;

    Scheduler::RegisterProcess(proc);
    return proc;
}

FancyRefPtr<Process> Process::create_kernel_process(void* entry, const char* name, Process* parent) {
    FancyRefPtr<Process> proc = new Process(Scheduler::GetNextPID(), name, "/", parent);

    proc->m_mainThread->registers.rip = reinterpret_cast<uintptr_t>(entry);
    proc->m_mainThread->registers.rsp = reinterpret_cast<uintptr_t>(proc->m_mainThread->kernelStack);
    proc->m_mainThread->registers.rbp = reinterpret_cast<uintptr_t>(proc->m_mainThread->kernelStack);

    proc->m_mainThread->kernelLock = 1;

    Scheduler::RegisterProcess(proc);
    return proc;
}

ErrorOr<FancyRefPtr<Process>> Process::create_elf_process(const FancyRefPtr<File>& elf, const Vector<String>& argv,
                                                        const Vector<String>& envp, const char* execPath,
                                                        Process* parent) {
    ELFData exe;
    TRY(elf_load_file(elf, exe));

    const char* name = "unknown";
    if (argv.size() >= 1) {
        name = argv[0].c_str();
    }
    FancyRefPtr<Process> proc = new Process(Scheduler::GetNextPID(), name, "/", parent);

    Thread* thread = proc->m_mainThread.get();
    thread->registers.cs = USER_CS; // We want user mode so use user mode segments, make sure RPL is 3
    thread->registers.ss = USER_SS;
    thread->timeSliceDefault = THREAD_TIMESLICE_DEFAULT;
    thread->timeSlice = thread->timeSliceDefault;
    thread->priority = 4;

    Error e = elf_load_segments(proc.get(), exe, 0);
    elf_free_data(exe);

    if (e != ERROR_NONE) {
        proc->die();
        delete proc->addressSpace;
        proc->addressSpace = nullptr;

        return e;
    }

    MappedRegion* stackRegion =
        proc->addressSpace->AllocateAnonymousVMObject(0x400000, 0x7000000000, false); // 4MB max stacksize

    thread->stack = reinterpret_cast<void*>(stackRegion->base); // 4MB stack size
    thread->registers.rsp = (uintptr_t)thread->stack + 0x400000;
    thread->registers.rbp = (uintptr_t)thread->stack + 0x400000;

    // Force the first 12KB to be allocated
    stackRegion->vmObject->Hit(stackRegion->base, 0x400000 - 0x1000, proc->get_page_map());
    stackRegion->vmObject->Hit(stackRegion->base, 0x400000 - 0x2000, proc->get_page_map());
    stackRegion->vmObject->Hit(stackRegion->base, 0x400000 - 0x3000, proc->get_page_map());

    proc->map_user_shared_data();
    proc->map_process_environment_block();

    thread->gsBase = proc->pebRegion->Base();
    Log::Info("pebbase %x", thread->gsBase);

    auto r = proc->load_elf(&thread->registers.rsp, exe, argv, envp, execPath);
    if (r.HasError()) {
        proc->die();
        delete proc->addressSpace;
        proc->addressSpace = nullptr;

        return r.Err();
    }

    thread->registers.rip = r.Value();

    assert(!(thread->registers.rsp & 0xF));

    // Reserve 3 file descriptors for stdin, out and err
    FsNode* nullDev = fs::ResolvePath("/dev/null");
    FsNode* logDev = fs::ResolvePath("/dev/kernellog");

    if (nullDev) {
        proc->m_handles[0] = MakeHandle(0, fs::Open(nullDev).Value()); // stdin
    } else {
        Log::Warning("Failed to find /dev/null");
    }

    if (logDev) {
        proc->m_handles[1] = MakeHandle(1, fs::Open(logDev).Value()); // stdout
        proc->m_handles[2] = MakeHandle(2, fs::Open(logDev).Value()); // stderr
    } else {
        Log::Warning("Failed to find /dev/kernellog");
    }

    Scheduler::RegisterProcess(proc);
    return proc;
}

void Process::kill_all_other_threads() {
    ScopedSpinLock lock{m_processLock};

    Thread* thisThread = Thread::current();
    assert(thisThread->parent == this);

    FancyRefPtr<Thread> thisThreadRef;

    List<FancyRefPtr<Thread>> runningThreads;
    for (auto& thread : m_threads) {
        if (thread == thisThread) {
            thisThreadRef = thread;
        } else if (thread) {
            asm("sti");
            acquireLockIntDisable(&thread->stateLock);
            if (thread->state == ThreadStateDying) {
                releaseLock(&thread->stateLock);
                asm("sti");
            }

            if (thread->blocker && thread->state == ThreadStateBlocked) {
                thread->state = ThreadStateZombie;
                releaseLock(&thread->stateLock);
                asm("sti");

                thread->blocker->interrupt(); // Stop the thread from blocking

                acquireLockIntDisable(&thread->stateLock);
            }

            thread->state = ThreadStateZombie;

            if (!acquireTestLock(&thread->kernelLock)) {
                thread->state =
                    ThreadStateDying; // We have acquired the lock so prevent the thread from getting scheduled
                thread->timeSlice = thread->timeSliceDefault = 0;

                releaseLock(&thread->stateLock);
                asm("sti");
            } else {
                releaseLock(&thread->stateLock);
                asm("sti");

                runningThreads.add_back(thread);
            }
        }
    }

    asm("sti");
    Log::Debug(debugLevelScheduler, DebugLevelNormal, "[%d] Killing threads...", m_pid);
    while (runningThreads.get_length()) {
        auto it = runningThreads.begin();
        while (it != runningThreads.end()) {
            assert(it->get()->state != ThreadStateRunning);

            FancyRefPtr<Thread> thread = *it;
            if (!acquireTestLock(
                    &thread->kernelLock)) { // Loop through all of the threads so we can acquire their locks
                acquireLockIntDisable(&thread->stateLock);
                thread->state = ThreadStateDying;
                thread->timeSlice = thread->timeSliceDefault = 0;
                releaseLock(&thread->stateLock);
                asm("sti");

                runningThreads.remove(it);

                it = runningThreads.begin();
            } else {
                it++;
            }
        }

        Scheduler::Yield();
    }

    assert(!runningThreads.get_length());

    m_mainThread = thisThreadRef;

    // Remove all the threads that were just killed
    for (auto it = m_threads.begin(); it != m_threads.end(); it++) {
        if ((*it) != thisThreadRef) {
            m_threads.remove(it);

            it = m_threads.begin();
        }
    }
}

Process::Process(pid_t pid, const char* _name, const char* _workingDir, Process* parent)
    : m_pid(pid), m_parent(parent) {
    if (_workingDir) {
        strncpy(workingDirPath, _workingDir, PATH_MAX);
    } else {
        strcpy(workingDirPath, "/");
    }

    FsNode* wdNode = fs::ResolvePath(workingDirPath);
    assert(wdNode);
    assert(wdNode->is_directory());

    if (auto openFile = wdNode->Open(0); !openFile.HasError()) {
        workingDir = std::move(openFile.Value());
    } else {
        assert(!openFile.HasError());
    }

    strncpy(name, _name, NAME_MAX);

    addressSpace = new AddressSpace(Memory::CreatePageMap());

    // Initialize signal handlers
    for (unsigned i = 0; i < SIGNAL_MAX; i++) {
        signalHandlers[i] = {
            .action = SignalHandler::HandlerAction::Default,
            .flags = 0,
            .mask = 0,
            .userHandler = nullptr,
        };
    }

    creationTime = Timer::GetSystemUptimeStruct();

    m_mainThread = new Thread(this, m_nextThreadID++);
    m_threads.add_back(m_mainThread);

    assert(m_mainThread->parent == this);

    m_handles.add_back(HANDLE_NULL); // stdin
    m_handles.add_back(HANDLE_NULL); // stdout
    m_handles.add_back(HANDLE_NULL); // stderr
}

ErrorOr<uintptr_t> Process::load_elf(uintptr_t* stackPointer, ELFData& elfInfo, const Vector<String>& argv,
                                    const Vector<String>& envp, const char* execPath) {
    uintptr_t rip = elfInfo.entry;
    uintptr_t linkerBaseAddress = 0x7FC0000000; // Linker base address
    ELFData interpreter;

    if (elfInfo.linkerPath) {
        // char* linkPath = elfInfo.linkerPath;

        FsNode* node = fs::ResolvePath("/lib/ld.so");
        if (!node) {
            KernelPanic("Failed to load dynamic linker!");
        }

        auto open = fs::Open(node);
        if (open.HasError()) {
            return open.Err();
        }

        FancyRefPtr<File> file = open.Value();

        Error e = elf_load_file(file, interpreter);
        if (e) {
            elf_free_data(interpreter);
            return e;
        }

        e = elf_load_segments(this, interpreter, linkerBaseAddress);
        rip = interpreter.entry;
    }

    char* tempArgv[argv.size()];
    char* tempEnvp[envp.size()];

    // ABI Stuff
    uint64_t* stack = (uint64_t*)(*stackPointer);

    asm("cli");
    asm volatile("mov %%rax, %%cr3" ::"a"(this->get_page_map()->pml4Phys));

    initialize_peb();

    ProcessEnvironmentBlock* peb = (ProcessEnvironmentBlock*)m_mainThread->gsBase;
    if (interpreter.dynamic.size()) {
        elf64_symbol_t* symtab = nullptr;
        elf64_rela_t* plt = nullptr;
        size_t pltSz = 0;
        char* strtab = nullptr;

        for (const elf64_dynamic_t& dynamic : interpreter.dynamic) {
            if (dynamic.tag == DT_PLTRELSZ) {
                pltSz = dynamic.val;
            } else if (dynamic.tag == DT_STRTAB) {
                strtab = (char*)(linkerBaseAddress + dynamic.ptr);
            } else if (dynamic.tag == DT_SYMTAB) {
                symtab = (elf64_symbol_t*)(linkerBaseAddress + dynamic.ptr);
            } else if (dynamic.tag == DT_JMPREL) {
                plt = (elf64_rela_t*)(linkerBaseAddress + dynamic.ptr);
            }
        }

        assert(plt && symtab && strtab);

        elf64_rela_t* pltEnd = (elf64_rela_t*)((uintptr_t)plt + pltSz);
        while (plt < pltEnd) {
            if (ELF64_R_TYPE(plt->info) == ELF64_R_X86_64_JUMP_SLOT) {
                long symIndex = ELF64_R_SYM(plt->info);
                elf64_symbol_t sym = symtab[symIndex];

                if (!sym.name) {
                    plt++;
                    continue;
                }

                int binding = ELF64_SYM_BIND(sym.info);
                assert(binding == STB_WEAK);

                char* name = strtab + sym.name;
                if (auto* s = ResolveHirakuSymbol(name)) {
                    uintptr_t* p = (uintptr_t*)(linkerBaseAddress + plt->offset);

                    *p = peb->hirakuBase + s->address + plt->addend;
                } else {
                    Log::Error("Failed to resolve program interpreter symbol %s", name);
                }
            }

            plt++;
        }
    } else {
        Log::Info("no dynamic??");
    }

    char* stackStr = (char*)stack;
    for (int i = 0; i < argv.size(); i++) {
        stackStr -= argv[i].Length() + 1;
        tempArgv[i] = stackStr;
        strcpy((char*)stackStr, argv[i].c_str());
    }

    for (int i = 0; i < envp.size(); i++) {
        stackStr -= envp[i].Length() + 1;
        tempEnvp[i] = stackStr;
        strcpy((char*)stackStr, envp[i].c_str());
    }

    char* execPathValue = nullptr;
    if (execPath) {
        stackStr -= strlen(execPath) + 1;
        strcpy((char*)stackStr, execPath);

        execPathValue = stackStr;
    }

    stackStr -= (uintptr_t)stackStr & 0xf; // align the stack

    stack = (uint64_t*)stackStr;

    stack -= ((argv.size() + envp.size()) % 2); // If argc + envc is odd then the stack will be misaligned

    stack--;
    *stack = 0; // AT_NULL

    stack -= sizeof(auxv_t) / sizeof(*stack);
    *((auxv_t*)stack) = {.a_type = AT_PHDR, .a_val = elfInfo.pHdrSegment}; // AT_PHDR

    stack -= sizeof(auxv_t) / sizeof(*stack);
    *((auxv_t*)stack) = {.a_type = AT_PHENT, .a_val = elfInfo.phEntrySize}; // AT_PHENT

    stack -= sizeof(auxv_t) / sizeof(*stack);
    *((auxv_t*)stack) = {.a_type = AT_PHNUM, .a_val = elfInfo.phNum}; // AT_PHNUM

    stack -= sizeof(auxv_t) / sizeof(*stack);
    *((auxv_t*)stack) = {.a_type = AT_ENTRY, .a_val = elfInfo.entry}; // AT_ENTRY

    stack -= sizeof(auxv_t) / sizeof(*stack);
    *((auxv_t*)stack) = {.a_type = AT_SYSINFO_EHDR, .a_val = peb->hirakuBase}; // AT_ENTRY

    /*if (execPath && execPathValue) {
        stack -= sizeof(auxv_t) / sizeof(*stack);
        *((auxv_t*)stack) = {.a_type = AT_EXECPATH, .a_val = (uint64_t)execPathValue}; // AT_EXECPATH
    }*/

    stack--;
    *stack = 0; // null

    stack -= envp.size();
    for (int i = 0; i < envp.size(); i++) {
        *(stack + i) = (uint64_t)tempEnvp[i];
    }

    stack--;
    *stack = 0; // null

    stack -= argv.size();
    for (int i = 0; i < argv.size(); i++) {
        *(stack + i) = (uint64_t)tempArgv[i];
    }

    stack--;
    *stack = argv.size(); // argc

    assert(!((uintptr_t)stack & 0xf));

    asm volatile("mov %%rax, %%cr3" ::"a"(Scheduler::GetCurrentProcess()->get_page_map()->pml4Phys));
    asm("sti");

    elf_free_data(interpreter);

    *stackPointer = (uintptr_t)stack;
    return rip;
}

Error Process::execve(ELFData& exe, const Vector<String>& argv, const Vector<String>& envp, const char* execPath) {
    ScopedSpinLock lock{m_processLock};

    RegisterContext* r = Thread::current()->scRegisters;

    assert(Process::current() == this);

    auto* oldSpace = addressSpace;
    auto* newSpace = new AddressSpace(Memory::CreatePageMap());

    asm("cli");
    addressSpace = newSpace;
    pebRegion = nullptr;
    userSharedDataRegion = nullptr;
    asm volatile("mov %%rax, %%cr3; sti" ::"a"(newSpace->get_page_map()->pml4Phys));

    delete oldSpace;

    Error e = elf_load_segments(this, exe, 0);
    if(e != ERROR_NONE) {
        return e;
    }

    Thread* t = Thread::current();
    MappedRegion* stack = addressSpace->AllocateAnonymousVMObject(0x400000, 0x7000000000, false); // 4MB max stacksize

    t->stack = (void*)stack->Base();
    r->rsp = stack->Base() + 0x400000;
    r->rbp = stack->Base() + 0x400000;

    // Force the first 12KB to be allocated
    stack->vmObject->Hit(stack->base, 0x400000 - 0x1000, addressSpace->get_page_map());
    stack->vmObject->Hit(stack->base, 0x400000 - 0x2000, addressSpace->get_page_map());
    stack->vmObject->Hit(stack->base, 0x400000 - 0x3000, addressSpace->get_page_map());

    map_user_shared_data();
    map_process_environment_block();

    t->gsBase = pebRegion->Base();

    auto ip = load_elf(&r->rsp, exe, argv, envp, execPath);
    if (ip.HasError()) {
        return ip.Err();
    }

    if(argv.size() > 0) {
        strncpy(name, argv[0].c_str(), 255);
    }

    r->rip = ip.Value();

    assert(!(r->rsp & 0xF));

    r->rflags = 0x202; // IF - Interrupt Flag, bit 1 should be 1
    memset(t->fxState, 0, 4096);

    ((fx_state_t*)t->fxState)->mxcsr = 0x1f80; // Default MXCSR (SSE Control Word) State
    ((fx_state_t*)t->fxState)->mxcsrMask = 0xffbf;
    ((fx_state_t*)t->fxState)->fcw = 0x33f; // Default FPU Control Word State

    // Restore default FPU state
    asm volatile("fxrstor64 (%0)" ::"r"((uintptr_t)t->fxState) : "memory");

    return ERROR_NONE;
}

Process::~Process() {
    ScopedSpinLock lock(m_processLock);
    assert(m_state == Process_Dead);
    assert(!m_parent);

    if (addressSpace) {
        delete addressSpace;
        addressSpace = nullptr;
    }
}

void Process::die() {
    asm volatile("sti");

    assert(m_state == Process_Running);
    Log::Debug(debugLevelScheduler, DebugLevelNormal, "Killing Process %s (PID %d)", name, m_pid);

    // Check if we are main thread
    Thread* thisThread = Thread::current();
    if (thisThread != thisThread->parent->get_main_thread().get()) {
        acquireLock(&m_processLock);
        if (m_state != Process_Dying) {
            // Kill the main thread
            thisThread->parent->get_main_thread()->signal(SIGKILL);
        }

        assert(thisThread->parent == this);

        asm volatile("cli");
        releaseLock(&m_processLock);
        releaseLock(&thisThread->kernelLock);
        asm volatile("sti");
        for (;;)
            Scheduler::Yield();
    }
    m_state = Process_Dying;

    // Ensure the current thread's lock is acquired
    assert(acquireTestLock(&thisThread->kernelLock));

    acquireLock(&m_processLock);
    List<FancyRefPtr<Thread>> runningThreads;

    for (auto& thread : m_threads) {
        if (thread != thisThread && thread) {
            asm("sti");
            acquireLockIntDisable(&thread->stateLock);
            if (thread->state == ThreadStateDying) {
                releaseLock(&thread->stateLock);
                asm("sti");
            }

            if (thread->blocker && thread->state == ThreadStateBlocked) {
                thread->state = ThreadStateZombie;
                releaseLock(&thread->stateLock);
                asm("sti");

                thread->blocker->interrupt(); // Stop the thread from blocking

                acquireLockIntDisable(&thread->stateLock);
            }

            thread->state = ThreadStateZombie;

            if (!acquireTestLock(&thread->kernelLock)) {
                thread->state =
                    ThreadStateDying; // We have acquired the lock so prevent the thread from getting scheduled
                thread->timeSlice = thread->timeSliceDefault = 0;

                releaseLock(&thread->stateLock);
                asm("sti");
            } else {
                releaseLock(&thread->stateLock);
                asm("sti");

                runningThreads.add_back(thread);
            }
        }
    }

    Log::Debug(debugLevelScheduler, DebugLevelNormal, "[%d] Killing child processes...", m_pid);
    while (m_children.get_length()) {
        // Ensure we release this at least momentarily in case threads are waiting on said lock
        releaseLock(&m_processLock);
        asm("sti");

        FancyRefPtr<Process> child = m_children.get_front();
        Log::Debug(debugLevelScheduler, DebugLevelVerbose, "[%d] Killing %d (%s)...", pid(), child->pid(), child->name);
        if (child->state() == Process_Running) {
            child->get_main_thread()->signal(SIGKILL); // Kill it, burn it with fire
            while (child->state() != Process_Dead)
                Scheduler::Yield(); // Wait for it to die
        } else if (child->state() == Process_Dying) {
            KernelObjectWatcher w;
            child->Watch(&w, KOEvent::ProcessTerminated);

            bool wasInterrupted = w.wait(); // Wait for the process to die
            while (wasInterrupted) {
                wasInterrupted = w.wait(); // If the parent tried to interrupt us we are dying anyway
            }
        }

        child->m_parent = nullptr;
        acquireLock(&m_processLock);
        m_children.remove(child); // Remove from child list
    }

    // Ensure we release this at least momentarily in case threads are waiting on said lock
    releaseLock(&m_processLock);

    asm("sti");
    Log::Debug(debugLevelScheduler, DebugLevelNormal, "[%d] Killing threads...", m_pid);
    while (runningThreads.get_length()) {
        auto it = runningThreads.begin();
        while (it != runningThreads.end()) {
            FancyRefPtr<Thread> thread = *it;
            if (!acquireTestLock(
                    &thread->kernelLock)) { // Loop through all of the threads so we can acquire their locks
                acquireLockIntDisable(&thread->stateLock);
                thread->state = ThreadStateDying;
                thread->timeSlice = thread->timeSliceDefault = 0;
                releaseLock(&thread->stateLock);
                asm("sti");

                runningThreads.remove(it);

                it = runningThreads.begin();
            } else {
                it++;
            }
        }

        thisThread->sleep(50000); // Sleep for 50 ms so we do not chew through CPU time
    }

    assert(!runningThreads.get_length());

    // Prevent run queue balancing
    acquireLock(&Scheduler::processesLock);
    acquireLockIntDisable(&m_processLock);

    CPU* cpu = GetCPULocal();
    APIC::Local::SendIPI(cpu->id, ICR_DSH_OTHER, ICR_MESSAGE_TYPE_FIXED, IPI_SCHEDULE);

    for (auto& t : m_threads) {
        assert(t->parent == this);
        if (t->state != ThreadStateDying && t != thisThread) {
            Log::Error("Thread (%s : %x) TID: %d should be dead, Current Thread (%s : %x) TID: %d", t->parent->name,
                       t.get(), t->tid, thisThread->parent->name, thisThread, thisThread->tid);
            KernelPanic("Thread should be dead");
        }
    }

    acquireLock(&cpu->runQueueLock);

    for (unsigned j = 0; j < cpu->runQueue->get_length(); j++) {
        if (Thread* thread = cpu->runQueue->get_at(j); thread != cpu->currentThread && thread->parent == this) {
            cpu->runQueue->remove_at(j);
            j = 0;
        }
    }

    releaseLock(&cpu->runQueueLock);

    for (unsigned i = 0; i < SMP::processorCount; i++) {
        if (i == cpu->id)
            continue; // Is current processor?

        CPU* other = SMP::cpus[i];
        asm("sti");
        acquireLockIntDisable(&other->runQueueLock);

        if (other->currentThread && other->currentThread->parent == this) {
            assert(other->currentThread->state == ThreadStateDying); // The thread state should be blocked

            other->currentThread = nullptr;
        }

        for (unsigned j = 0; j < other->runQueue->get_length(); j++) {
            Thread* thread = other->runQueue->get_at(j);

            assert(thread);

            if (thread->parent == this) {
                other->runQueue->remove(thread);
                j = 0;
            }
        }

        if (other->currentThread == nullptr) {
            APIC::Local::SendIPI(i, ICR_DSH_SELF, ICR_MESSAGE_TYPE_FIXED, IPI_SCHEDULE);
        }

        releaseLock(&other->runQueueLock);
        asm("sti");

        if (other->currentThread == nullptr) {
            APIC::Local::SendIPI(i, ICR_DSH_SELF, ICR_MESSAGE_TYPE_FIXED, IPI_SCHEDULE);
        }
    }

    asm("sti");
    releaseLock(&Scheduler::processesLock);

    Log::Debug(debugLevelScheduler, DebugLevelNormal, "[%d] Closing handles...", m_pid);
    m_handles.clear();

    Log::Debug(debugLevelScheduler, DebugLevelNormal, "[%d] Signaling watchers...", m_pid);
    {
        ScopedSpinLock lockWatchers(m_watchingLock);

        // All threads have ceased, set state to dead
        m_state = Process_Dead;

        for (auto* watcher : m_watching) {
            watcher->signal();
        }
        m_watching.clear();
    }

    if (m_parent && (m_parent->state() == Process_Running)) {
        Log::Debug(debugLevelScheduler, DebugLevelNormal, "[%d] Sending SIGCHILD to %s...", m_pid, m_parent->name);
        m_parent->get_main_thread()->signal(SIGCHLD);
    }

    // Add to destroyed processes so the reaper thread can safely destroy any last resources
    Log::Debug(debugLevelScheduler, DebugLevelNormal, "[%d] Marking process for destruction...", m_pid);
    Scheduler::MarkProcessForDestruction(this);

    bool isDyingProcess = (thisThread->parent == this);
    if (isDyingProcess) {
        acquireLockIntDisable(&cpu->runQueueLock);
        Log::Debug(debugLevelScheduler, DebugLevelNormal, "[%d] Rescheduling...", m_pid);

        asm volatile("mov %%rax, %%cr3" ::"a"(((uint64_t)Memory::kernelPML4) - KERNEL_VIRTUAL_BASE));

        thisThread->state = ThreadStateDying;
        thisThread->timeSlice = 0;

        releaseLock(&m_processLock);

        // cpu may have changes since we released the processes lock
        // as the run queue
        cpu = GetCPULocal();
        cpu->runQueue->remove(thisThread);
        cpu->currentThread = cpu->idleThread;

        releaseLock(&cpu->runQueueLock);

        Scheduler::DoSwitch(cpu);
        KernelPanic("Dead process attempting to continue execution");
    } else {
        releaseLock(&m_processLock);
    }
}

void Process::start() {
    ScopedSpinLock acq(m_processLock);
    assert(!m_started);

    Scheduler::InsertNewThreadIntoQueue(m_mainThread.get());
    m_started = true;
}

void Process::Watch(KernelObjectWatcher* watcher, KOEvent events) {
    assert(HAS_KOEVENT(events, KOEvent::ProcessTerminated));

    ScopedSpinLock acq(m_watchingLock);

    if (m_state == Process_Dead) {
        watcher->signal();
        return; // Process is already dead
    }

    m_watching.add_back(watcher);
}

void Process::Unwatch(KernelObjectWatcher* watcher) {
    ScopedSpinLock acq(m_watchingLock);

    if (m_state == Process_Dead) {
        return; // Should already be removed from watching
    }

    m_watching.remove(watcher);
}

FancyRefPtr<Process> Process::fork() {
    assert(this == Process::current());

    ScopedSpinLock lock(m_processLock);

    FancyRefPtr<Process> newProcess = new Process(Scheduler::GetNextPID(), name, workingDirPath, this);
    delete newProcess->addressSpace; // TODO: Do not create address space in first place
    newProcess->addressSpace = addressSpace->fork();

    PageMap* thisPageMap = this->get_page_map();
    PageMap* otherPageMap = newProcess->get_page_map();

    // Force TLB flush
    asm volatile("mov %%rax, %%cr3" ::"a"(thisPageMap->pml4Phys));

    newProcess->euid = euid;
    newProcess->uid = uid;
    newProcess->euid = egid;
    newProcess->gid = gid;

    newProcess->m_handles.resize(m_handles.size());
    for (unsigned i = 0; i < m_handles.size(); i++) {
        newProcess->m_handles[i] = m_handles[i];
    }

    m_children.add_back(newProcess);

    acquireLock(addressSpace->GetLock());

    // Unmap the old PEB and allocate a new one
    if (pebRegion) {
        newProcess->addressSpace->UnmapMemory(pebRegion->Base(), pebRegion->Size());
    }

    // The base of the user shared data region should be identical
    newProcess->userSharedDataRegion = newProcess->addressSpace->AddressToRegionReadLock(userSharedDataRegion->Base());
    if (!newProcess->userSharedDataRegion) {
        Log::Warning("[%d : %s] User shared data region may have been unmapped.", m_pid, name);
        newProcess->map_user_shared_data();
    } else {
        newProcess->userSharedDataRegion->lock.release_read();
    }

    releaseLock(addressSpace->GetLock());

    newProcess->map_process_environment_block();
    newProcess->m_mainThread->gsBase = newProcess->pebRegion->Base();

    // TODO: Make it so that the process cannot unmap the PEB

    newProcess->pebRegion->vmObject->MapAllocatedBlocks(newProcess->pebRegion->Base(), otherPageMap);

    assert(CheckInterrupts());
    asm("cli");

    assert(newProcess->userSharedDataRegion);
    asm volatile("mov %%rax, %%cr3" ::"a"(otherPageMap->pml4Phys));
    newProcess->initialize_peb();
    asm volatile("mov %%rax, %%cr3" ::"a"(thisPageMap->pml4Phys));
    asm("sti");

    Scheduler::RegisterProcess(newProcess);
    return newProcess;
}

FancyRefPtr<Thread> Process::create_child_thread(void* entry, void* stack) {
    ScopedSpinLock lock{m_processLock};

    pid_t threadID = m_nextThreadID++;
    FancyRefPtr<Thread> thread = m_threads.add_back(new Thread(this, threadID));

    thread->registers.rip = (uintptr_t)entry;
    thread->registers.rsp = (uintptr_t)stack;
    thread->registers.rbp = (uintptr_t)stack;
    thread->state = ThreadStateRunning;
    thread->stack = thread->stackLimit = reinterpret_cast<void*>(stack);

    RegisterContext* registers = &thread->registers;
    registers->rflags = 0x202; // IF - Interrupt Flag, bit 1 should be 1
    thread->registers.cs = USER_CS;
    thread->registers.ss = USER_SS;
    thread->timeSliceDefault = THREAD_TIMESLICE_DEFAULT;
    thread->timeSlice = thread->timeSliceDefault;
    thread->priority = 4;

    Scheduler::InsertNewThreadIntoQueue(thread.get());
    return thread;
}

FancyRefPtr<Thread> Process::GetThreadFromTID_Unlocked(pid_t tid) {
    for (const FancyRefPtr<Thread>& t : m_threads) {
        if (t->tid == tid) {
            return t;
        }
    }

    return nullptr;
}

void Process::map_user_shared_data() {
    FancyRefPtr<UserSharedData> userSharedData;
    {
        ScopedSpinLock<true> lockUSD(userSharedDataLock);
        if (!userSharedDataVMO) {
            userSharedDataVMO = new FancyRefPtr(new UserSharedData());
        }

        userSharedData = *userSharedDataVMO;
    }

    userSharedDataRegion =
        addressSpace->MapVMO(static_pointer_cast<VMObject>(userSharedData), PROC_USER_SHARED_DATA_BASE, true);
    assert(userSharedDataRegion);

    // Allocate space for both a siginfo struct and the signal trampoline
    m_signalTrampoline = addressSpace->AllocateAnonymousVMObject(
        ((signalTrampolineEnd - signalTrampolineStart) + PAGE_SIZE_4K - 1) & ~static_cast<unsigned>(PAGE_SIZE_4K - 1),
        0x7000A00000, false);
    reinterpret_cast<PhysicalVMObject*>(m_signalTrampoline->vmObject.get())
        ->ForceAllocate(); // Forcibly allocate all blocks
    m_signalTrampoline->vmObject->MapAllocatedBlocks(m_signalTrampoline->Base(), get_page_map());

    // Copy signal trampoline code into process
    asm volatile("cli; mov %%rax, %%cr3" ::"a"(get_page_map()->pml4Phys));
    memcpy(reinterpret_cast<void*>(m_signalTrampoline->Base()), signalTrampolineStart,
           signalTrampolineEnd - signalTrampolineStart);
    asm volatile("mov %%rax, %%cr3; sti" ::"a"(Scheduler::GetCurrentProcess()->get_page_map()->pml4Phys));
}

void Process::map_process_environment_block() {
    pebRegion = addressSpace->AllocateAnonymousVMObject(PAGE_COUNT_4K(sizeof(ProcessEnvironmentBlock)) << PAGE_SHIFT_4K,
                                                        PROC_PEB_BASE, false);
    pebRegion->vmObject->Hit(pebRegion->base, 0, get_page_map());
}

void Process::initialize_peb() {
    ProcessEnvironmentBlock* peb = (ProcessEnvironmentBlock*)m_mainThread->gsBase;
    peb->self = peb;
    peb->pid = m_pid;
    peb->executableBaseAddress = 0x80000000;
    peb->sharedDataBase = userSharedDataRegion->Base();
    peb->sharedDataSize = userSharedDataRegion->Size();
    peb->hirakuBase = userSharedDataRegion->Base() + (_hiraku - _user_shared_data);
    peb->hirakuSize = (_hiraku_end - _hiraku);
}
