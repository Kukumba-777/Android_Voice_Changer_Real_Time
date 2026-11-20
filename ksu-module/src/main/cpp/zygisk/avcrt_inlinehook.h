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
//   • PAC (paciasp/pacibsp) entry, or any PC-relative instruction in the copy region (ADR/ADRP/
//     B/BL/B.cond/CB*/TB*/LDR-literal) → SKIP (return nullptr) — we can't copy those verbatim, and
//     skipping is safe (voice just isn't changed in that app) vs. crashing the process.
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
#include <cstring>

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
// PAC sign-LR entries — copying them verbatim would re-sign LR with the wrong SP context → skip.
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
                               const char** detail = nullptr) {
    auto set_reason = [&](int r) { if (reason) *reason = r; };
    if (detail) *detail = nullptr;

    const uint32_t* code = static_cast<const uint32_t*>(target);
    uint32_t i0 = code[0];

    // Decide the lead-in: preserve a BTI landing pad; bail on PAC; nothing for a plain prologue.
    size_t lead = 0;
    if (insn_is_bti(i0)) {
        lead = 4;
    } else if (insn_is_pac_entry(i0)) {
        set_reason(HOOK_UNSAFE_PROLOGUE);
        if (detail) *detail = "PAC entry";
        return nullptr;  // PAC entry — can't safely replicate the sign-LR context
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
    if (!avcrt_write_code(target, patch, copy_len)) {
        munmap(tramp, tramp_len);
        set_reason(HOOK_WRITE_FAILED);
        return nullptr;
    }
    set_reason(HOOK_OK);
    return tramp;
}

}  // namespace avcrt
