// Codegen / memory-unsafety — issues #4157, #4158, #4169: three per-lowering
// state members left unprotected across `Emitter::buildSpecializedMethodFn`'s
// own NESTED state-machine emission (`LlvmBackend_Calls2.cpp`, ~L13133),
// audited and filed together because they are the same defect class at the
// SAME call site as `fix #4118` (`asyncChainedReturn_` / `asyncBorrowedReturn_`
// unprotected in `lowerLambda`'s async branch — the sibling site).
//
// THE SITE. `buildSpecializedMethodFn` lazily monomorphizes a generic
// async/iterator method's specialized body MID-EMISSION of an enclosing
// state-machine method's own body (its own comment names the hazard: "a
// lazily-built specialization ... emits its OWN state machine ... which
// overwrites the shared state-machine context"). It already guards the whole
// `BEZEL_SM_FRAME_MEMBERS` frame with `SmFrameSaveScope`, and separately saves
// `smIterCleanupAtState_` / `asyncHandlerSpillSlots_` by name. Three more
// members reachable from this exact site had neither treatment:
//
//   #4157 — asyncChainedReturn_ / asyncBorrowedReturn_ (the §30.5 Pattern A
//           return-kind flags; #4118's OWN pair, at a FOURTH site #4118 did
//           not touch). DESTRUCTIVE: they select which runtime intrinsic
//           (bz_task_set_chained vs bz_task_set_result_disposer) is emitted.
//           Fixed here by mirroring #4118's own by-name save/reset(false)/
//           restore treatment, NOT a macro fold: they are ALSO hand-declared
//           members of `NestedBodyScope` (LlvmBackend_impl.hpp ~L12696-97),
//           so folding them into BEZEL_SM_FRAME_MEMBERS would duplicate-
//           declare them there (a hard compile error -- verified with a real
//           `-fsyntax-only` run of the would-be macro edit before choosing
//           this shape).
//
//   #4158 — iterElemIsRuntimeManagedPrimitive_ (a sibling of #3505's six
//           originally-protected members: the SAME exhaustive three-way
//           `yield return` dispose-previous-value dispatch as the
//           already-protected iterElemIsOwnedClass_ / iterIsOwningIter_).
//           DESTRUCTIVE: gates whether the previously-yielded runtime-managed
//           primitive (non-nullable `string`) is disposed before the next
//           `yield return` overwrites the slot -- a leaked `false` skips a
//           disposal that should fire (LANE_RULES §"soft-hint vs destructive"
//           classification: this selects a real runtime action, so it is
//           destructive, not a mere hint).
//
//   #4169 — smIterEnumFrameSlot_ (maps a for-in loop's enumerator/source
//           alloca to its state-machine frame field, so the iterator's
//           Dispose can route an abandonment-time cleanup through the FIELD
//           rather than a raw alloca from a different function invocation).
//           DESTRUCTIVE: selects which of two disposal-routing mechanisms an
//           abandoned for-in resource gets (IterCleanup::Kind::EnumFrame vs
//           the node-keyed fallback); losing the mapping drops the
//           EnumFrame-routed cleanup for an OUTER iterator's own for-in
//           resource once a nested generic iterator specialization clobbers
//           the (until now unprotected) shared map.
//
// #4158 and #4169, UNLIKE #4157's pair, have no such NestedBodyScope
// collision (verified: `git grep -n
// 'iterElemIsRuntimeManagedPrimitive_\|smIterEnumFrameSlot_'
// src/codegen/LlvmBackend_Assign.cpp` returns nothing), so both are folded
// directly into `BEZEL_SM_FRAME_MEMBERS` (LlvmBackend_impl.hpp) -- ONE
// mechanism (`SmFrameSaveScope`, already constructed at this exact site)
// covers both, with no new code needed in `buildSpecializedMethodFn` itself:
// both members are write-before-read WITHIN the single emitter call that
// establishes them (confirmed by inspection: `iterElemIsRuntimeManagedPrimitive_`
// is set unconditionally near the top of `emitIteratorMethod`'s MoveNext body,
// LlvmBackend_Iterators.cpp ~L2221, before the body that reads it lowers;
// `smIterEnumFrameSlot_` is `.clear()`-ed at the top of every one of the four
// MoveNext/Resume emitters before any write into it), so a plain SAVE (no
// pre-call reset) at buildSpecializedMethodFn's entry and a RESTORE at its
// exit is sufficient -- exactly what folding into the macro's existing
// SmFrameSaveScope already does automatically.
//
// EVERY EXIT PATH. `buildSpecializedMethodFn` has exactly ONE `return` after
// the save point (`return specFn;` at the function's end) -- the only earlier
// `return` (`return cit->second;`, the cache-hit early-out) is BEFORE the
// save/reset block runs. Confirmed by grep across the whole function body.
// So the #4157 by-name restore, placed once in the function's single
// "Restore outer state" section, covers every exit; #4158/#4169's macro fold
// is covered by `SmFrameSaveScope`'s own destructor regardless.
//
// ORACLE. Each row's observable is a live-object disposal counter
// (destructor-decremented static `n`, exactly the #3505 / #4118 convention)
// under BOTH the plain AOT oracle (a swapped disposer is a heap-use-after-free
// -> SIGSEGV, `ran=false`) and ASan/LeakSanitizer (a skipped disposal is a
// LEAK; a swapped disposer is a heap-use-after-free) -- neither oracle
// dominates (#2633). Leak rows are loop-amplified per LANE_RULES (200-300
// iterations) so a one-shot run cannot pass vacuously on both sides.
//
// MEASURED (isolated probe compiles against this exact diff, both directions):
//   #4157 (heap-use-after-free, chained task double-dispose):
//     pre-fix : plain SIGSEGV-class ASan abort --
//       "heap-use-after-free ... READ of size 8 ... in bz$elemDispose"
//       freed by bz$M.C4157$ChainAsync, allocated inside the NESTED
//       Gen4157$P$spec / Gen4157$Produce$spec (the lazy specialization).
//     post-fix: clean.
//   #4158 (leak, displaced string not disposed):
//     pre-fix : LeakSanitizer, 10500 B / 600 allocations, from
//       bz$iter$M.C4158$Gen4158$MoveNext (300 iterations x [1 direct + 1
//       indirect] leaked string allocation).
//     post-fix: clean, exit code matches the expected per-iteration total.
//   #4169 (leak, outer for-in enumerator/source frame not disposed on
//   abandonment):
//     pre-fix : LeakSanitizer, 16000 B / 400 allocations, from
//       bz$iter$M.C4169$Src$GetEnumerator and bz$M.C4169$Src (200 iterations
//       x [enumerator + source] leaked).
//     post-fix: clean, Widget4169.n == 0 (every created widget disposed).
//
// Spec: §29.1 / §29.2 / §29.7 (iterator disposal protocol), §30.5 (async
// return-kind detection), v0.295.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

// Plain (non-instrumented) AOT run. A SIGSEGV is WIFSIGNALED -> ran=false.
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan+LSan run: a heap-use-after-free / leak surfaces as a nonzero abort
// that the harness reports via ran=false, or as an exit code that does not
// match the program's own clean-path return value.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent -> case skipped
    return r.ran ? r.exitCode : -999;
}

// -----------------------------------------------------------------------
// #4157 — asyncChainedReturn_ / asyncBorrowedReturn_
// -----------------------------------------------------------------------

const char* kWidget4157 =
    "class Widget4157 "
    "  public static n: int := 0; "
    "  public constructor() Widget4157.n := Widget4157.n + 1; end constructor; "
    "  destructor Widget4157.n := Widget4157.n - 1; end destructor; "
    "end Widget4157; ";

constexpr int kIters4157 = 200;

// THE ROW. `ChainAsync` does `return await Gen4157<int>.Produce();` --
// genuinely §30.5 kind-2, chained onto the FIRST-TIME generic specialization
// `Gen4157<int>.Produce`, which is itself genuinely kind-2 (chained onto
// `Gen4157<int>.P()`'s real, non-null task). Building `Gen4157<int>.Produce`
// for the first time routes through `buildSpecializedMethodFn` MID-EMISSION
// of `ChainAsync`'s own Resume body (the operand of its `return await`), so
// the specialization's own read/reset of asyncChainedReturn_ (for ITS OWN,
// separately-correct kind-2 classification) leaks into ChainAsync's
// subsequent read without the save/reset/restore this fix adds.
std::string program4157() {
    return std::string("module M; ") + kWidget4157 +
        "class Gen4157<T> "
        "  public static async method Produce(): Task<Widget4157> "
        "    return await Gen4157<T>.P(); "
        "  end Produce; "
        "  public static async method P(): Task<Widget4157> "
        "    return new Widget4157(); "
        "  end P; "
        "end Gen4157; "
        "class C4157 "
        "  public static async method ChainAsync(): Task<Widget4157> "
        "    return await Gen4157<int>.Produce(); "
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters4157) + " do "
        "      do "
        "        using var outer := C4157.ChainAsync(); "
        "      end; "
        "    end; "
        "    return Widget4157.n; "
        "  end Main; "
        "end C4157;";
}

// CONTROL — the identical chained-return shape, but `Produce` is a PLAIN
// (non-generic) method on a non-generic class: no generic specialization is
// ever built, so `buildSpecializedMethodFn` (and this fix's new code inside
// it) is never reached. Must be byte-for-byte identical before and after.
std::string control4157() {
    return std::string("module M; ") + kWidget4157 +
        "class Plain4157 "
        "  public static async method Produce(): Task<Widget4157> "
        "    return await Plain4157.P(); "
        "  end Produce; "
        "  public static async method P(): Task<Widget4157> "
        "    return new Widget4157(); "
        "  end P; "
        "end Plain4157; "
        "class C4157b "
        "  public static async method ChainAsync(): Task<Widget4157> "
        "    return await Plain4157.Produce(); "
        "  end ChainAsync; "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters4157) + " do "
        "      do "
        "        using var outer := C4157b.ChainAsync(); "
        "      end; "
        "    end; "
        "    return Widget4157.n; "
        "  end Main; "
        "end C4157b;";
}

// -----------------------------------------------------------------------
// #4158 — iterElemIsRuntimeManagedPrimitive_
// -----------------------------------------------------------------------

constexpr int kIters4158 = 300;

// THE ROW. `Gen4158` is a BORROWING iterator over `string` (a runtime-
// managed primitive), so iterElemIsRuntimeManagedPrimitive_ is TRUE for its
// own MoveNext body. Between its two `yield return <+1 producer>` statements
// it calls `Inner<bool>(3)` for the FIRST time -- a generic ITERATOR method,
// so building its specialization mid-Gen4158's-own-body routes through
// `buildSpecializedMethodFn` -> `emitIteratorMethod`, which sets
// iterElemIsRuntimeManagedPrimitive_ = false for ITS OWN (int) element type.
// Without this fix's protection, that clobbers Gen4158's own `true`, so the
// SECOND yield's "dispose the displaced previous value" branch is skipped:
// the first yielded string leaks.
std::string program4158() {
    return "module M; class C4158 "
        "  public static method Inner<T>(n: int): IEnumerable<int> "
        "    for var i in 1..n do yield return i; end; "
        "  end Inner; "
        "  public static method Gen4158(): IEnumerable<string> "
        "    yield return \"a\" + \"1\"; "
        "    var junk := 0; "
        "    for var i in Inner<bool>(3) do junk := junk + i; end; "
        "    yield return \"b\" + \"2\"; "
        "  end Gen4158; "
        "  public static method Drive4158(): int "
        "    var n := 0; "
        "    for var s in Gen4158() do n := n + 1; end; "
        "    return n; "
        "  end Drive4158; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var k in 0..<" + std::to_string(kIters4158) + " do "
        "      total := total + Drive4158(); "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C4158;";
}
// Expected clean total: 2 yields consumed per Drive4158() call, kIters4158
// times, truncated to the AOT harness's 8-bit exit code (§ runet-vs-exit).
int expected4158() {
    return (kIters4158 * 2) & 0xFF;
}

// CONTROL — the identical shape, but the nested first-time generic
// specialization call is REMOVED (replaced with a plain, non-generic-
// inducing statement). buildSpecializedMethodFn's nested-emission path is
// never exercised for a runtime-managed-primitive iterator here, so this
// row must be identical before and after the fix.
std::string control4158() {
    return "module M; class C4158b "
        "  public static method Gen4158b(): IEnumerable<string> "
        "    yield return \"a\" + \"1\"; "
        "    var junk := 0; "
        "    for var i in 1..3 do junk := junk + i; end; "
        "    yield return \"b\" + \"2\"; "
        "  end Gen4158b; "
        "  public static method Drive4158b(): int "
        "    var n := 0; "
        "    for var s in Gen4158b() do n := n + 1; end; "
        "    return n; "
        "  end Drive4158b; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var k in 0..<" + std::to_string(kIters4158) + " do "
        "      total := total + Drive4158b(); "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C4158b;";
}

// -----------------------------------------------------------------------
// #4169 — smIterEnumFrameSlot_
// -----------------------------------------------------------------------

const char* kWidget4169 =
    "class Widget4169 "
    "  public static n: int := 0; "
    "  public constructor() Widget4169.n := Widget4169.n + 1; end constructor; "
    "  destructor Widget4169.n := Widget4169.n - 1; end destructor; "
    "end Widget4169; ";

constexpr int kIters4169 = 200;

// THE ROW. `Gen` is an iterator whose `for var x in Src() do ... end;` loop
// CONTAINS a `yield return` (so Src()'s enumerator/source must be
// frame-spilled to survive a suspend inside the loop -- establishing an
// entry in smIterEnumFrameSlot_). Before the yield, it calls `Inner<bool>(2)`
// for the first time (a generic ITERATOR method), routing through
// buildSpecializedMethodFn -> emitIteratorMethod, whose OWN entry does an
// unconditional `smIterEnumFrameSlot_.clear()`. Without this fix's
// protection, that wipes Gen's own just-established enumerator->frame-field
// mapping; abandoning the iterator (via `using var` disposal after exactly
// one MoveNext, while still suspended inside the Src() for-in loop) then
// cannot route the cleanup through the frame field, and the outer Src()
// enumerator/source resource leaks.
std::string program4169() {
    return std::string("module M; ") + kWidget4169 +
        "class C4169 "
        "  public static method Src(): IEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "    yield return 3; "
        "  end Src; "
        "  public static method Inner<T>(n: int): IEnumerable<int> "
        "    for var i in 1..n do yield return i; end; "
        "  end Inner; "
        "  public static method Gen(): IEnumerable<Widget4169> "
        "    for var x in Src() do "
        "      var j := 0; "
        "      for var i in Inner<bool>(2) do j := j + i; end; "
        "      yield return new Widget4169(); "
        "    end; "
        "  end Gen; "
        "  public static method Drive(): int "
        "    using var en := Gen().GetEnumerator(); "
        "    var ok := en.MoveNext(); "
        "    if ok then return 1; end; "
        "    return 0; "
        "  end Drive; "
        "  public static method Main(): int "
        "    for var k in 0..<" + std::to_string(kIters4169) + " do "
        "      var d := Drive(); "
        "    end; "
        "    return Widget4169.n; "
        "  end Main; "
        "end C4169;";
}

// CONTROL — the identical abandonment-inside-a-suspending-for-in shape, but
// the nested first-time generic iterator specialization is REMOVED (replaced
// with a plain range sum). smIterEnumFrameSlot_ is still established and
// consulted for Src()'s own enumerator, but buildSpecializedMethodFn's
// nested path is never exercised, so this row must be identical before and
// after the fix.
std::string control4169() {
    return std::string("module M; ") + kWidget4169 +
        "class C4169b "
        "  public static method Src(): IEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "    yield return 3; "
        "  end Src; "
        "  public static method Gen(): IEnumerable<Widget4169> "
        "    for var x in Src() do "
        "      var j := 0; "
        "      for var i in 1..2 do j := j + i; end; "
        "      yield return new Widget4169(); "
        "    end; "
        "  end Gen; "
        "  public static method Drive(): int "
        "    using var en := Gen().GetEnumerator(); "
        "    var ok := en.MoveNext(); "
        "    if ok then return 1; end; "
        "    return 0; "
        "  end Drive; "
        "  public static method Main(): int "
        "    for var k in 0..<" + std::to_string(kIters4169) + " do "
        "      var d := Drive(); "
        "    end; "
        "    return Widget4169.n; "
        "  end Main; "
        "end C4169b;";
}

}  // namespace

// #4157: pre-fix SIGSEGVs (plain) / heap-use-after-frees (ASan) on the very
// first iteration; post-fix clean, Widget4157.n == 0.
BEZEL_TEST(buildspecmethodfn_unprotected_members_4157_4158_4169,
           chained_return_first_time_generic_spec_swaps_disposer_4157) {
    const std::string src = program4157();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL — no generic specialization reached; must be clean on both sides.
BEZEL_TEST(buildspecmethodfn_unprotected_members_4157_4158_4169,
           chained_return_no_generic_spec_control_4157) {
    const std::string src = control4157();
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// #4158: pre-fix leaks one displaced string per Drive4158() call (measured
// 10500 B / 600 allocations at kIters4158=300); post-fix clean.
BEZEL_TEST(buildspecmethodfn_unprotected_members_4157_4158_4169,
           runtime_managed_primitive_flag_clobbered_by_nested_spec_4158) {
    const std::string src = program4158();
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == expected4158() || c == -1000);
}

// CONTROL — no generic specialization reached; must be clean and match the
// same expected total on both sides.
BEZEL_TEST(buildspecmethodfn_unprotected_members_4157_4158_4169,
           runtime_managed_primitive_no_generic_spec_control_4158) {
    const std::string src = control4158();
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == expected4158() || c == -1000);
}

// #4169: pre-fix leaks the outer for-in enumerator+source on abandonment
// (measured 16000 B / 400 allocations at kIters4169=200); post-fix clean,
// Widget4169.n == 0.
BEZEL_TEST(buildspecmethodfn_unprotected_members_4157_4158_4169,
           enum_frame_slot_clobbered_by_nested_spec_on_abandonment_4169) {
    const std::string src = program4169();
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}

// CONTROL — no generic specialization reached; must be clean on both sides.
BEZEL_TEST(buildspecmethodfn_unprotected_members_4157_4158_4169,
           enum_frame_slot_no_generic_spec_control_4169) {
    const std::string src = control4169();
    const int c = runChecked(src);
    BEZEL_EXPECT_TRUE(c == 0 || c == -1000);
}
