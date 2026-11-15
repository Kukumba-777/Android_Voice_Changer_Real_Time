// avcrt_inject — minimal root arm64 ptrace remote-dlopen injector.
//
//   avcrt_inject <pid> <absolute-.so-path>
//
// Attaches to <pid>, makes it call dlopen(<path>, RTLD_NOW) by hijacking a thread's registers, then
// restores the original registers and detaches. Used to load libavcrt_afhook.so into the audioserver
// process (a non-zygote init service Zygisk can't reach). Must run as root; on this device SELinux is
// permissive so ptrace of audioserver is allowed (an enforcing device would need sepolicy).
//
// The target's linker namespace ([system], isolated) only loads from its search paths, so <path> must
// be under /system/lib64 (staged there by the Magisk module's magic-mount), NOT /data.
//
// Mechanism: dlopen's address in the target = target_libdl_base + (our_dlopen_off_in_libdl). We set
// x0=path, x1=RTLD_NOW, LR(x30)=0 so the call faults (SIGSEGV) on return → we regain control and read
// x0 = the dlopen handle. caller_addr is null (LR=0) → the linker uses the default [system] namespace,
// which can load /system/lib64. Then we restore the saved registers and detach — the target resumes
// exactly where it was, now with our .so loaded (its constructor installs the hook).

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

// arm64 user_pt_regs layout (uapi asm/ptrace.h): 31 GPRs + sp + pc + pstate.
struct Regs64 {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

static const int RTLD_NOW_FLAG = 2;

// Lowest mapped address of the first mapping whose path contains `name`, in /proc/<pid>/maps.
static uintptr_t module_base(pid_t pid, const char *name) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/maps", pid);
    FILE *f = fopen(p, "re");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, name)) {
            base = (uintptr_t)strtoull(line, nullptr, 16);
            break;
        }
    }
    fclose(f);
    return base;
}

static bool get_regs(pid_t pid, Regs64 *r) {
    struct iovec io = {r, sizeof(*r)};
    return ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &io) == 0;
}
static bool set_regs(pid_t pid, Regs64 *r) {
    struct iovec io = {r, sizeof(*r)};
    return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &io) == 0;
}

// Write `len` bytes from `src` to the target's address `dst` (must be a writable mapping, e.g. stack).
static bool write_mem(pid_t pid, uintptr_t dst, const void *src, size_t len) {
    struct iovec local = {(void *)src, len};
    struct iovec remote = {(void *)dst, len};
    return process_vm_writev(pid, &local, 1, &remote, 1, 0) == (ssize_t)len;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <pid> <abs-so-path>\n", argv[0]);
        return 2;
    }
    pid_t pid = (pid_t)atoi(argv[1]);
    const char *so = argv[2];
    size_t so_len = strlen(so) + 1;

    // Resolve dlopen in the target by file offset within libdl.so (same file in both processes).
    uintptr_t our_libdl = module_base(getpid(), "libdl.so");
    void *our_dlopen = dlsym(RTLD_DEFAULT, "dlopen");   // real libdl address (not our PLT stub)
    uintptr_t tgt_libdl = module_base(pid, "libdl.so");
    if (!our_libdl || !our_dlopen || !tgt_libdl) {
        fprintf(stderr, "resolve fail: our_libdl=%p our_dlopen=%p tgt_libdl=%p\n",
                (void *)our_libdl, our_dlopen, (void *)tgt_libdl);
        return 1;
    }
    uintptr_t remote_dlopen = tgt_libdl + ((uintptr_t)our_dlopen - our_libdl);
    printf("our_libdl=0x%lx our_dlopen=0x%lx tgt_libdl=0x%lx remote_dlopen=0x%lx\n",
           our_libdl, (uintptr_t)our_dlopen, tgt_libdl, remote_dlopen);

    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) != 0) { perror("ATTACH"); return 1; }
    int st;
    waitpid(pid, &st, 0);

    Regs64 saved, work;
    if (!get_regs(pid, &saved)) { perror("GETREGSET"); ptrace(PTRACE_DETACH, pid, 0, 0); return 1; }
    work = saved;

    // Stage the path string on the stack, well below sp; keep 16-byte alignment.
    uintptr_t str_addr = (saved.sp - 1024) & ~(uintptr_t)15;
    if (!write_mem(pid, str_addr, so, so_len)) {
        perror("write path"); ptrace(PTRACE_DETACH, pid, 0, 0); return 1;
    }

    work.regs[0] = str_addr;                       // x0 = filename
    work.regs[1] = RTLD_NOW_FLAG;                   // x1 = RTLD_NOW
    work.regs[30] = 0;                              // LR = 0 → fault on return (we regain control)
    work.sp = (str_addr - 512) & ~(uintptr_t)15;    // clean 16-aligned stack below the string
    work.pc = remote_dlopen;
    if (!set_regs(pid, &work)) { perror("SETREGSET"); ptrace(PTRACE_DETACH, pid, 0, 0); return 1; }

    // Run dlopen; it returns to PC=0 → SIGSEGV → the target stops and we inspect x0 (the handle).
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);

    uint64_t handle = 0;
    if (WIFSTOPPED(st)) {
        Regs64 after;
        if (get_regs(pid, &after)) handle = after.regs[0];
        int sig = WSTOPSIG(st);
        printf("stopped sig=%d (%s) dlopen handle=0x%lx\n", sig, strsignal(sig), (unsigned long)handle);
    } else {
        printf("unexpected wait status 0x%x\n", st);
    }

    // Restore the original registers so the target resumes exactly where it was, then detach.
    set_regs(pid, &saved);
    ptrace(PTRACE_DETACH, pid, 0, 0);

    if (handle == 0) { fprintf(stderr, "dlopen FAILED (handle 0) — check path/namespace\n"); return 1; }
    printf("INJECTED OK: %s -> handle 0x%lx in pid %d\n", so, (unsigned long)handle, pid);
    return 0;
}
