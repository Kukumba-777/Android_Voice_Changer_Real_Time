// avcrt_inlinehook — a minimal, SELF-CHECKING AArch64 inline hooker.
//
// We do NOT use a general inline-hook library: shadowhook is dead on Android 16 (its linker
// hook needs a namespace-blocked libshadowhook_nothing.so) and Dobby's only release doesn't
// compile for Android. Both ship a full instruction RELOCATOR (ADRP/ADR/literal/branch fixups)
// — the big, risky part. We deliberately DON'T relocate: instead we copy the overwritten
// prologue verbatim, and to stay safe on ANY device/ROM we VERIFY at runtime that those bytes
// are copy-safe, gracefully SKIPPING the hook (never crashing) when they aren't.
//
// Autonomy across devices: a different ROM's libaudioclient prologue may differ from the one we
// verified. So avcrt_inline_hook() decodes the prologue and:
//   • normal prologue (sub/stp/mov/… — PC-independent) → 16-byte absolute-jump hook (the common,
//     verified case);
//   • BTI landing pad first (bti c/j/jc) → PRESERVE it (20-byte patch) so indirect callers don't
//     fault on BTI-enforced builds;
//   • PAC (paciasp/pacibsp) entry → opt-in single-instruction hook after the signing instruction;
//     the bridge authenticates before entering the replacement, and the original signs again.
//   • Unsupported prologues or PC-relative copies are skipped.
//
// Mechanism (classic absolute-jump inline hook), lead = 0 (normal) or 4 (preserved BTI pad):
//   target:  overwrite [0, lead+16) with  [orig pad?] LDR x16,#8 ; BR x16 ; .quad replacement
//   tramp:   [orig bytes 0..lead+16) verbatim] + LDR x16,#8 ; BR x16 ; .quad target+lead+16
// x16 (IP0) is a caller-scratch reg, dead at function entry → safe to clobber in the jump.

#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>   // off64_t
#include <unistd.h>      // pwrite64, getpagesize
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace avcrt {

// avcrt_inline_hook reason codes (out param), for caller logging.
enum HookReason { HOOK_OK = 0, HOOK_UNSAFE_PROLOGUE = 1, HOOK_WRITE_FAILED = 2, HOOK_NOMEM = 3 };

constexpr size_t kJumpLen = 16;  // LDR x16,#8 (4) + BR x16 (4) + .quad (8)

// True if `i` cannot be copied to another address verbatim (its encoding is PC-relative).
inline bool insn_is_pc_relative(uint32_t i) {
    if ((i & 0x1F000000u) == 0x10000000u) return true;  // ADR / ADRP
    if ((i & 0x7C000000u) == 0x14000000u) return true;  // B / BL
    if ((i & 0x7E000000u) == 0x34000000u) return true;  // CBZ / CBNZ
    if ((i & 0x7E000000u) == 0x36000000u) return true;  // TBZ / TBNZ
    if ((i & 0xFE000000u) == 0x54000000u) return true;  // B.cond / BC.cond
    if ((i & 0x3B000000u) == 0x18000000u) return true;  // LDR (literal), LDRSW/PRFM/SIMD literal
    return false;
}

// BTI landing pads (stateless hints) — safe to preserve/copy.
inline bool insn_is_bti(uint32_t i) {
    return i == 0xD503245Fu /*bti c*/ || i == 0xD503243Fu /*bti j*/ || i == 0xD503247Fu /*bti jc*/;
}
inline bool insn_is_pac_entry(uint32_t i) {
    return i == 0xD503233Fu /*paciasp*/ || i == 0xD503237Fu /*pacibsp*/;
}

// Build a 16-byte absolute jump `LDR x16,#8 ; BR x16 ; .quad dest` at `out`.
inline void avcrt_make_abs_jump(void* out, void* dest) {
    uint32_t insns[2] = {0x58000050u /*LDR x16,#8*/, 0xD61F0200u /*BR x16*/};
    uint8_t buf[kJumpLen];
    memcpy(buf, insns, sizeof(insns));
    uint64_t d = reinterpret_cast<uint64_t>(dest);
    memcpy(buf + 8, &d, 8);
    memcpy(out, buf, kJumpLen);
}

#if defined(__aarch64__)
inline uint32_t avcrt_direct_branch(uintptr_t from, uintptr_t to) {
    constexpr uintptr_t reach = 1u << 27; // A64 B: signed imm26 in instruction words.
    if ((from | to) & 3u) return 0;
    if (to >= from ? to - from >= reach : from - to > reach) return 0;
    return 0x14000000u | (static_cast<uint32_t>((to - from) >> 2) & 0x03ffffffu);
}

inline bool avcrt_pac_stack_setup(uint32_t word) {
    return (word & 0xff8003ffu) == 0xd10003ffu || // sub sp, sp, #imm
           (word & 0xffc07fffu) == 0xa9807bfdu;   // stp x29, x30, [sp, #imm]!
}

// Keep the loader's BTI state when changing permissions on the code page.
inline int avcrt_read_exec_prot(uintptr_t pc) {
    FILE* file = std::fopen("/proc/self/smaps", "re");
    if (!file) return 0;
    char line[512];
    bool found = false;
    int prot = 0;
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long begin, end;
        char mode[5];
        if (std::sscanf(line, "%lx-%lx %4s", &begin, &end, mode) == 3) {
            if (found) break;
            found = pc >= begin && pc < end;
            if (found && std::strcmp(mode, "r-xp") != 0) break;
        } else if (found && std::strncmp(line, "VmFlags:", 8) == 0) {
            prot = PROT_READ | PROT_EXEC;
            char* state = nullptr;
            for (const char* flag = strtok_r(line + 8, " \t\n", &state); flag;
                 flag = strtok_r(nullptr, " \t\n", &state)) {
                if (std::strcmp(flag, "bt") == 0) prot |= PROT_BTI;
            }
            break;
        }
    }
    std::fclose(file);
    return prot;
}

inline void* avcrt_pac_hook(const uint32_t* pac, void* replacement, int* reason,
                            const char** detail, void (*publish_original)(void*)) {
    auto fail = [&](int r, const char* why) -> void* {
        if (reason) *reason = r;
        if (detail) *detail = why;
        return nullptr;
    };
    const auto saved = pac[1];
    if (!avcrt_pac_stack_setup(saved)) return fail(HOOK_UNSAFE_PROLOGUE, "unsupported PAC frame setup");
    auto* slot = const_cast<uint32_t*>(pac + 1);
    const auto pc = reinterpret_cast<uintptr_t>(slot);
    const int prot = avcrt_read_exec_prot(pc);
    if (!prot) return fail(HOOK_WRITE_FAILED, "PAC code page protections unknown");
    const auto page_size = static_cast<uintptr_t>(getpagesize());
    const auto page = pc & ~(page_size - 1);

    // No fixed mappings. Use hints within B reach and verify the actual addresses.
    void* mem = MAP_FAILED;
    uint32_t redirect = 0, resume = 0;
    for (uintptr_t step = 0; step < (1u << 27); step += (1u << 21)) {
        for (bool above : {true, false}) {
            if ((!above && step == 0) || (!above && page <= step)) continue;
            const auto hint = above ? page + step : page - step;
            mem = mmap(reinterpret_cast<void*>(hint), page_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mem == MAP_FAILED) continue;
            const auto addr = reinterpret_cast<uintptr_t>(mem);
            redirect = avcrt_direct_branch(pc, addr + 16);
            resume = avcrt_direct_branch(addr + 12, pc + 4);
            if (redirect && resume) break;
            munmap(mem, page_size);
            mem = MAP_FAILED;
        }
        if (mem != MAP_FAILED) break;
    }
    if (mem == MAP_FAILED) return fail(HOOK_NOMEM, "no PAC trampoline within branch reach");

    const uint32_t code[] = {0xd503245fu /* bti c */, pac[0], saved, resume,
                            pac[0] == 0xd503233fu ? 0xd50323bfu : 0xd50323ffu /* autiasp/autibsp */,
                            0xd503201fu /* nop; align the jump's address literal */};
    memcpy(mem, code, sizeof(code));
    avcrt_make_abs_jump(static_cast<uint8_t*>(mem) + sizeof(code), replacement);
    if (mprotect(mem, page_size, prot) != 0) {
        munmap(mem, page_size);
        return fail(HOOK_NOMEM, "PAC trampoline not executable");
    }
    __builtin___clear_cache(static_cast<char*>(mem), static_cast<char*>(mem) + sizeof(code) + kJumpLen);

    auto* code_page = reinterpret_cast<void*>(page);
    if (mprotect(code_page, page_size, prot | PROT_WRITE) != 0) {
        munmap(mem, page_size);
        return fail(HOOK_WRITE_FAILED, "PAC code page not writable");
    }
    if (publish_original) publish_original(mem);
    // A running caller sees either the original frame setup or the branch.
    // Never overwrite live instructions with a multiword PAC patch.
    __atomic_store_n(slot, redirect, __ATOMIC_RELEASE);
    __builtin___clear_cache(reinterpret_cast<char*>(slot), reinterpret_cast<char*>(slot + 1));
    const bool restored = mprotect(code_page, page_size, prot) == 0;
    if (reason) *reason = HOOK_OK;
    if (detail) *detail = restored ? "PAC entry bridge" : "PAC entry bridge (code page RX restore failed)";
    return mem;
}
#endif

// Write `len` bytes to executable code at `addr`. Tries mprotect(RWX)+memcpy; falls back to
// /proc/self/mem pwrite64 (kernel force-write through a RO mapping — the debugger mechanism),
// which sidesteps SELinux execmod denials. Flushes i-cache.
inline bool avcrt_write_code(void* addr, const void* data, size_t len) {
    const uintptr_t ps = static_cast<uintptr_t>(getpagesize());
    const uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~(ps - 1);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(addr) + len + ps - 1) & ~(ps - 1);
    const size_t span = static_cast<size_t>(end - page);
    bool ok = false;

    if (mprotect(reinterpret_cast<void*>(page), span, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        memcpy(addr, data, len);
        mprotect(reinterpret_cast<void*>(page), span, PROT_READ | PROT_EXEC);
        ok = true;
    } else {
        int fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            if (pwrite64(fd, data, len, static_cast<off64_t>(reinterpret_cast<uintptr_t>(addr)))
                == static_cast<ssize_t>(len)) {
                ok = true;
            }
            close(fd);
        }
    }
    if (ok) __builtin___clear_cache(reinterpret_cast<char*>(addr),
                                    reinterpret_cast<char*>(addr) + len);
    return ok;
}

// Inline-hook `target` to call `replacement`. Returns a trampoline (call it with the original
// signature to run the original) on success, or nullptr if the prologue isn't copy-safe / the
// code write failed. `*reason` (optional) explains a nullptr.
inline void* avcrt_inline_hook(void* target, void* replacement, int* reason = nullptr,
                               const char** detail = nullptr,
                               void (*publish_original)(void*) = nullptr,
                               bool allow_pac = false) {
    auto set_reason = [&](int r) { if (reason) *reason = r; };
    if (detail) *detail = nullptr;

    const uint32_t* code = static_cast<const uint32_t*>(target);
    uint32_t i0 = code[0];

    const size_t lead = insn_is_bti(i0) ? 4 : 0;
    if (insn_is_pac_entry(code[lead / 4])) {
#if defined(__aarch64__)
        if (allow_pac) return avcrt_pac_hook(code + lead / 4, replacement, reason, detail, publish_original);
#else
        (void)allow_pac;
#endif
        set_reason(HOOK_UNSAFE_PROLOGUE);
        if (detail) *detail = "PAC entry";
        return nullptr;
    }

    // The 16-byte jump overwrites 4 instructions after the lead-in; all must be copy-safe.
    const size_t copy_len = lead + kJumpLen;          // bytes overwritten at target
    for (size_t k = lead / 4; k < copy_len / 4; ++k) {
        if (insn_is_pac_entry(code[k]) || insn_is_pc_relative(code[k])) {
            set_reason(HOOK_UNSAFE_PROLOGUE);
            if (detail) *detail = insn_is_pac_entry(code[k]) ? "PAC in copy region" : "PC-relative instruction";
            return nullptr;
        }
    }

    // Trampoline: [orig bytes 0..copy_len) verbatim] + [jump back to target+copy_len].
    const size_t tramp_len = copy_len + kJumpLen;
    void* tramp = mmap(nullptr, tramp_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) { set_reason(HOOK_NOMEM); return nullptr; }

    memcpy(tramp, target, copy_len);  // copy-safe (verified above; BTI pad is a stateless hint)
    avcrt_make_abs_jump(static_cast<uint8_t*>(tramp) + copy_len,
                        static_cast<uint8_t*>(target) + copy_len);
    if (mprotect(tramp, tramp_len, PROT_READ | PROT_EXEC) != 0) {
        munmap(tramp, tramp_len);
        set_reason(HOOK_NOMEM);
        return nullptr;
    }
    __builtin___clear_cache(static_cast<char*>(tramp), static_cast<char*>(tramp) + tramp_len);

    // Patch target: [preserved lead bytes] + [jump to replacement]. (memcpy of the lead from a
    // local copy of the original keeps it identical, e.g. bti c stays bti c.)
    uint8_t patch[4 + kJumpLen];
    if (lead) memcpy(patch, target, lead);
    avcrt_make_abs_jump(patch + lead, replacement);
    // Let callers publish the prepared trampoline before the replacement can
    // be reached. This does not synchronize the executable code overwrite.
    if (publish_original) publish_original(tramp);
    if (!avcrt_write_code(target, patch, copy_len)) {
        if (publish_original) publish_original(nullptr);
        munmap(tramp, tramp_len);
        set_reason(HOOK_WRITE_FAILED);
        return nullptr;
    }
    set_reason(HOOK_OK);
    return tramp;
}

}  // namespace avcrt
