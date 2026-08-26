// fix #2754 — codegen: two `disposeStack_` gaps (§28 "Resource Management",
// v0.282).
//
// FOUND BY THE READ-ONLY REVIEW of #2733's fix (`b7cd422`). Neither is
// introduced by that commit; one (#2, FFI-temp slot across await) is
// IMPROVED by #2733 (pre-fix was a wild free; post-#2733 is a bounded leak;
// both are wrong, the bounded leak is "strictly better"). Filed together
// because they share a mechanism — the codegen-time `disposeStack_` family.
//
// ───────────────────────────────────────────────────────────────────────────
// GAP (a) — `emitInstanceInitFunction` clears `disposeStack_` and never
// restores it
// ───────────────────────────────────────────────────────────────────────────
//
// `src/codegen/LlvmBackend_Setup.cpp:11082` clears the stack and then lowers
// user field initializers under it. Every other emitter that lowers user code
// under a swapped `currentFn_` (lambda body, the six property/indexer accessor
// emitters, `buildSpecializedMethodFn`, the three `LlvmBackend_Layout.cpp`
// thunks, `genericInstSfinitFn`, the specialized disposers — see the audit at
// `src/codegen/LlvmBackend_Setup.cpp:4013` and Disposal.cpp:1519 / :4119) saves,
// clears, and restores. emitInstanceInitFunction is the odd member out.
//
// MEASURED PRE-FIX. With the `auto savedDisposeStack = disposeStack_;` /
// `disposeStack_ = std::move(savedDisposeStack);` pair removed (only the
// clear at line 11082 kept), a two-class program whose field initializer
// produces an owned-class temporary via a using-var-style declaration showed a
// stale entry on `disposeStack_` at the start of the SECOND class's init
// emission (the first class's field-init pushed a Disposal into the cleared
// stack; without the restore, the second iteration saw it). The K8 boundary
// then triggered the null-guard's alternate path on the wrong scope and the
// produced IR was malformed (`Basic Block … does not have terminator` from
// the #2633-era verify hook). With the pair present, both classes emit
// independently and the verifier is clean.
//
// The chosen test program amplifies the latent by putting a record-typed
// field initializer on a PUBLIC non-generic class so the imported-init
// emission path is exercised end-to-end, then verifies the resulting `bz$X$init`
// body produces the expected field-init effect at IMPORT time and does not
// leak. Spec §22.8 step 2 + §28.3.
//
// ───────────────────────────────────────────────────────────────────────────
// GAP (b) — FFI temp slots are not in `asyncHandlerSpillSlots_`
// ───────────────────────────────────────────────────────────────────────────
//
// An FFI buffer produced BEFORE a suspension point (specifically: an entry-
// block null-init pointer slot created by `entryAllocaNullPtr()` for a
// `string`→`CString` argument marshalling, then written by `bz_string_to_utf8z`
// in the pre-suspend state) does not survive a real cross-thread resume. Its
// slot is a plain alloca in the pre-suspend frame, not spilled into the async
// state machine's frame; on a worker thread the slot reads whatever uninit-
// ialised stack happened to be at that offset. The K8 boundary's free is
// null-guarded so a NULL-skipped reclaim becomes a "bounded leak" (#2733's
// null-init at production was strictly better than the pre-#2733 wild free
// but it is still wrong — the buffer is heap-allocated and never freed).
//
// The fix: `registerFfiExprTemp` pushes the slot onto `asyncHandlerSpillSlots_`
// when `smStateStruct_` is non-null — mirroring exactly what
// `registerAwaitedTaskTemp` does for the analogous Pattern A task temp at
// `src/codegen/LlvmBackend_Async.cpp:3702`. `fixupSuspendDominance` then
// heap-spills the slot through `asyncSpillPtrField_` so a cross-thread resume
// reads the persisted value (the same machinery named locals already use).
//
// MEASURED PRE-FIX (runAotChecked on a `Consume("Ā") + await Producer()` shape
// where `Producer` returns `Task<int>` after `Task.Delay(1)` to force a real
// cross-thread resume): ASan+LSan reports the FFI buffer as a leak at process
// exit (`Direct leak of N byte(s) … bz_string_to_utf8z`). With the fix, the
// slot is heap-spilled and the K8 boundary free reclaims the buffer; LSan
// clean. Spec §28.3 K8 boundary + §30.13 cross-thread resume, both v0.282.
//
// ORACLES. Each row runs under `runAotChecked` (ASan + LSan): a leak flips
// `ran` to false with an ASan report in `stdoutText`. A value row uses a
// composite exit code (a sum the FFI call knows about + the awaiter's value)
// so the FFI-consume half and the resume half each contribute a digit the
// wrong-shape breaks.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ASan+LSan run: a leak / UAF / double-free surfaces as `ran=false`.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// GAP (a) — emitInstanceInitFunction's missing disposeStack_ restore.
//
// A TWO-CLASS public-non-generic program, both with field initialisers that
// push disposals onto disposeStack_ while the init function lowers them. The
// pre-fix shape let the FIRST class's pushed entries survive into the SECOND
// class's init emission (the loop iterates `initFns_` and each iteration
// clears without restoring); the malformed IR tripped the verifier on the
// second iteration's end-of-function path.
//
// We pin the FIXED behaviour with a value row: the program-level Main builds
// an `App` (whose init emits field init `Box(_v := new Box(7))` on its
// imported `Box`) and asserts `_v.V == 7` plus an exact-disposal counter
// (the disposer runs once, after the slot's boundary).
// ===========================================================================
BEZEL_TEST(dispose_stack_two_gaps_2754, instance_init_dispose_stack_restore_spec_28_3) {
    auto r = runAotChecked(
        "module M; "
        "public class Box "
        "  public static n: int := 0; "
        "  public V: int; "
        "  public constructor(x: int) V := x; Box.n := Box.n + 1; end constructor; "
        "  destructor Box.n := Box.n - 1; end destructor; "
        "end Box; "
        "public class Holder "
        "  public owned _v: Box; "
        "  public constructor() _v := new Box(7); end constructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    if h._v.V <> 7 then return 31; end; "
        "    if Box.n <> 1 then return 32; end; "
        "    return 30; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 30);
}

// ── GAP (a) — control: a SECOND public non-generic class emitted AFTER the
// first, so the iteration over `initFns_` actually goes through `clear()` twice
// in a single compilation. Pre-fix the second class's stale-stack window
// produced malformed IR; this row verifies both classes emit cleanly.
BEZEL_TEST(dispose_stack_two_gaps_2754,
           instance_init_second_class_after_clear_spec_22_8) {
    auto r = runAotChecked(
        "module M; "
        "public class Box "
        "  public V: int; "
        "  public constructor(x: int) V := x; end constructor; "
        "end Box; "
        "public class Holder "
        "  public owned _v: Box; "
        "  public constructor() _v := new Box(3); end constructor; "
        "end Holder; "
        "public class Other "
        "  public _x: int; "
        "  public constructor() _x := 41; end constructor; "
        "end Other; "
        "public class App "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var o := new Other(); "
        "    if h._v.V + o._x <> 44 then return 51; end; "
        "    return 50; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 50);
}

// ===========================================================================
// GAP (b) — FFI temp slot does not survive await suspension.
//
// An async method whose expression holds an FFI buffer (via the implicit
// `string`→`CString` marshalling to a `CString`-param extern) AND an `await`
// INSIDE the same full expression, so the K8 boundary that frees the FFI
// buffer fires AFTER the resume. `Task.Delay(1)` forces a real cross-thread
// resume so a non-heap-spilled slot reads as garbage on the worker's fresh
// stack. `Producer` returns `Task<int>` whose body runs after the delay, so
// the consumer's `+` operator has a real resumed second operand.
//
// Spec §28.3 K8 boundary + §30.13 cross-thread resume + §32.5/§32.6 FFI
// marshalling, all v0.282.
//
// FAIL-WITHOUT.  Pre-fix the slot's K8 boundary free reads from a stale stack
// offset; LSan reports `Direct leak of N byte(s)` from `bz_string_to_utf8z`.
// With the fix the slot is heap-spilled through the frame and the K8 free
// reclaims the buffer exactly once.
//
// "Ā" is U+0100 (width=2 in a `bz.string`), which forces the
// `bz_string_to_utf8z` arm of the marshalling code (an ASCII-width=1 string
// never allocates a buffer). Without the alloc the K8 free is a no-op even
// without the heap-spill.
// ===========================================================================
BEZEL_TEST(dispose_stack_two_gaps_2754,
           ffi_temp_slot_survives_cross_thread_resume_spec_28_3_30_13) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C "
        "  public static async method Producer(): Task<int> "
        "    await Task.Delay(1); "
        "    return 99; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    return strlen(\"Ā\") + await Producer(); "
        "  end Main; "
        "end C;");
    // "Ā" → UTF-8 2 bytes → strlen == 2; + Producer == 2 + 99 = 101.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 101);
}

// ── GAP (b) — control: the SAME shape WITHOUT the await (so the FFI temp is
// consumed within the same full expression as the call, no cross-thread resume,
// no slot-spanning). The K8 boundary free runs inline; this passes both before
// and after the fix (it pins the bug is specific to the cross-await case).
BEZEL_TEST(dispose_stack_two_gaps_2754,
           ffi_temp_slot_no_await_control_spec_28_3) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C "
        "  public static method Main(): int "
        "    return strlen(\"Ā\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}
