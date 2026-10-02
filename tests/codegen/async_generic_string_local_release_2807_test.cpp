// Codegen / refcounting — issue #2807: EVERY `var` string local declared in an
// ASYNC method body or in a GENERIC-SPECIALIZATION body leaked its reference.
// Two allocations (the `bz_string` header plus its data buffer) per string per
// call, unboundedly in a service loop. Nothing about the local mattered — no
// interface, no copy, no collection: the whole #429/#1997 string-local refcount
// machinery was simply inert in those two bodies.
//
// Spec: §4.6 "`string`"; §28.9 "Runtime-managed types" — "`string` — pervasive;
// created by literals, interpolation, concatenation, and library methods", and
// "The implementation of runtime-managed types' lifecycle (reference counting,
// region allocation, or other) is not part of this specification"; §28.3
// "Local variables, ownership, and borrows", kernel K1.6 (a runtime-managed
// value carries no classification and no obligation); §30.1 "`async` methods"
// and §30.2 "`await`" for the state-machine lowering; §14.1 "Generic
// declarations" for the monomorphized body. Spec revision v0.240.
//
// So none of these locals carries a §28 disposal obligation; what each carries,
// in THIS implementation, is one reference, and the observable requirement is
// that the heap string be reclaimed. It was not.
//
// THE DEFECT. Every consumer of the three string-eligibility sets gates on
// `currentFn_ == stringReleaseEligibleFn_`:
//   * armStringLocalRefcount            (LlvmBackend_Setup.cpp) — the bind-time
//     retain AND the scope-end Disposal{stringRelease}: the sole registrar;
//   * returnedStringLocalOwnsPlusOne    (LlvmBackend_Setup.cpp) — transfer-on-
//     return instead of a second mint;
//   * isTrackedStringLocalForRelease    (LlvmBackend_Assign.cpp / _Exprs.cpp) —
//     the record-backed interface-property retain;
//   * the `:=` and §4.6 `+=` drop-on-overwrite release (LlvmBackend_Stmts.cpp).
// `stringReleaseEligibleFn_` was assigned in exactly ONE place in the backend,
// emitFunctionBody — and emitFunctionBody's async routing RETURNS before that
// assignment runs, while a generic specialization is emitted by
// emitSpecializedMethod (LlvmBackend_Calls2.cpp), which never touched it at all.
// Both bodies therefore lowered with `currentFn_` naming a fresh llvm::Function
// while the token still named the last synchronous method emitted: the gate was
// false for every string local in every such body.
//
// THE FIX runs collectStringReleaseEligible for the body being emitted and pins
// the token to it — the same re-establishment emitAsyncMethod already performed
// for the closure (#488), unwind (#1896), capture-cell (#1416/#1418) and
// borrowing-collection (#2132/#2142/#2143) analyses, and the same one
// emitSpecializedMethod already performed for the unwind sets (#1483). The
// analysis is a pure walk of the body's own AST, so it describes the function
// being emitted; nothing is inherited from another function.
//
// WHY IT IS NOT A DOUBLE-FREE. Two mechanisms carry the weight:
//   * FRAME SPILL. collectSuspendLocals gives EVERY LocalDecl in a state-machine
//     body its own frame field, and emitAwaitSuspend spills / restores every
//     mapped alloca around each suspend — so the alloca the Disposal loads holds
//     the live value on whichever activation reaches the exit. A SUSPEND returns
//     from resume WITHOUT touching disposeStack_, so a suspended local is
//     released once, by the activation that finally completes (case C).
//   * CO-GATED PARAMETER RETAIN. Pinning the token also turns on the `:=` / `+=`
//     drop-on-overwrite release for a `string` PARAMETER (localSlot() resolves
//     one), whose single reference belongs to the CALLER — §28.1: on a `string`,
//     "`owned` is a compile error: such values carry no disposal obligation to
//     transfer (§28.9)". Releasing it unpaired is the #2696 use-after-free, so
//     retainAssignedStringParams is called in the SAME place. This was verified
//     to be load-bearing, not decorative: commenting out ONLY those two calls
//     and rebuilding turns cases (E) and (J) into
//     "AddressSanitizer: heap-use-after-free" (measured). They are the tests
//     that would catch a regression of the co-gate.
//
// ⚠ Every string here is built at RUNTIME (`$"as-{i}"`): `"ab" + "cd"`
// constant-folds to an rc sentinel on which bz_string_release no-ops — the trap
// that has made probes in this family vacuous repeatedly. That is also why the
// `Holder` field in (D)/(E)/(J)/(K) is assigned `$"hn-{seed}"` from a variable
// rather than given a literal initializer.
//
// ⚠ EVERY case is LOOP-AMPLIFIED and allocates a churn string per iteration.
// This is load-bearing: a ONE-call `Main` is NOT a reliable LSan oracle (a
// leftover pointer in a register makes LSan report a leaked block as reachable,
// so the same binary reports a leak on some runs and not others).
//
// ⚠ Cases (D) and (K) are EXACTLY-ONCE oracles, not merely leak checks: a
// BORROW local is retained at bind and released at scope end against the SAME
// heap string a live `Holder` field owns, 20 times, and the field is read again
// afterwards. An over-release frees it under the object — and the generated code
// is ASan-instrumented, so the read faults as heap-use-after-free; an
// under-release leaks it past the Holder's disposal (LSan). Both directions are
// observable in one program.
//
// MECHANICAL FAIL -> PASS on ce96c35 (`--sanitize=address`, per case). The PLAIN
// (unsanitized) value is IDENTICAL before and after in every one of the eleven
// programs — a leak is invisible without a sanitizer, so LSan is the oracle that
// moves, and the value assertions pin that the fix froze nothing early:
//   (A) 1904 B / 80 allocs -> clean, value 91    (G) 2167 B / 94  -> clean, 98
//   (B) 1904 B / 80        -> clean, value 91    (H) 2802 B / 120 -> clean, 164
//   (C) 1904 B / 80        -> clean, value 91    (K) 1153 B / 40  -> clean, 84
//   (D) 1153 B / 40        -> clean, value 84    (L)  371 B / 20  -> clean, 123
//   (F)  934 B / 40        -> clean, value 123
//   (E) 1446 B / 60 -> 390 B / 20   (J) 953 B / 40 -> 390 B / 20
// (E)/(J) are characterization cases: they still leak the parameter slot's FINAL
// value — #2696's documented, deliberate imbalance ("No scope-end release is
// registered for the parameter slot, so its final value leaks") — but the leak
// SHRINKS to exactly that, and no memory error appears.
//
// ⚠ DELIBERATE EXCLUSIONS at the time this test was written, each measured
// post-fix and each still a bounded leak with the correct value and NO memory
// error — the safe direction, filed as a follow-up (#2817) rather than fixed
// here, because each has its own activation model (an iterator resume runs once
// per advance and has a separate abandonment path; a lambda/constructor body is
// lowered mid-function with no body of its own to analyze at that point):
//   async iterator body   1904 B / 80 allocs   sync iterator body 1845 B / 78
//   plain lambda body     1904 B / 80          async lambda body  1904 B / 80
//   compiled constructor  1904 B / 80
// ALL FIVE were subsequently fixed by #2817 (v0.240), which pins the token in
// emitIteratorMethod, emitAsyncIteratorMethod, lowerLambda, emitAsyncLambda and
// emitConstructorBody (plus inlineCtorBody, whose unanalyzed body the
// constructor pin would otherwise have turned into a use-after-free). Case (M)
// below is the async-iterator one; its assertion moved from "must keep leaking"
// to "is clean" at that point — see its own comment.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

std::string sanitizerText(const JitResult& r) {
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    return all;
}

// A checked run that is expected to LEAK (a documented, pre-existing trade) and,
// in particular, NOT to have freed anything early. `r.ran` is false for a leak
// AND for a use-after-free, so the distinction has to be read out of the report.
void expectLeakButNoMemoryError(const JitResult& r) {
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") != std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("SEGV") == std::string::npos);
}

// sum over i in 1..20 of len("xx-{i}") = 9*4 + 11*5 = 91.
constexpr int kLenSum20 = 91;
// sum over i in 1..10 of len("xx-{i}") = 9*4 + 1*5 = 41.
constexpr int kLenSum10 = 41;

// The churn allocation every case makes. The `< 0` test is never true; it forces
// the read so the local cannot be elided, and the allocate/free pair means a
// prematurely freed block cannot be read back intact on the plain path.
#define CHURN                                                                  \
    "    var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "                    \
    "    if churn.Length < 0 then return 99; end; "

// A `Holder` whose `string` field is filled with a RUNTIME-built value (a
// literal initializer would be an rc sentinel and make the oracle vacuous).
#define HOLDER_DECL                                                            \
    "module M; class Holder "                                                  \
    "  public Name: string := \"\"; "                                          \
    "end Holder; "
#define HOLDER_INIT                                                            \
    "    using var h := new Holder(); "                                        \
    "    var seed := 7; "                                                      \
    "    h.Name := $\"hn-{seed}\"; "        /* len 4 */

// =====================================================================
// (A) THE ISSUE'S OWN REPRO A — two `+1` producer locals in a plain async
// method body, 20 calls. §30.1 / §4.6 / §28.9.
// =====================================================================
const char* kAsyncProducerLocals =
    "module M; class C "
    "  private static async method Work(i: int): Task<int> "
    "    var built := $\"as-{i}\"; "
    CHURN
    "    return built.Length; "
    "  end Work; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + await C.Work(i); "
    "    end; "
    "    return total; "                     // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (B) THE ISSUE'S OWN REPRO B — a producer local in a GENERIC-SPECIALIZATION
// body, 20 calls. §14.1 / §4.6 / §28.9.
// =====================================================================
const char* kGenericSpecProducerLocal =
    "module M; class Box<T> "
    "  public static method Copy(i: int): int "
    "    var built := $\"gn-{i}\"; "
    CHURN
    "    return built.Length; "
    "  end Copy; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + Box<string>.Copy(i); "
    "    end; "
    "    return total; "                     // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (C) THE FRAME-SPILL CASE — the local is bound BEFORE an `await` and read
// AFTER it, so the activation that releases it is not the one that bound it.
// Its alloca round-trips through the state-machine frame. §30.2 / §30.13.
// =====================================================================
const char* kAsyncLocalAcrossAwait =
    "module M; class C "
    "  private static async method Inner(i: int): Task<int> "
    "    return i; "
    "  end Inner; "
    "  private static async method Work(i: int): Task<int> "
    "    var built := $\"aw-{i}\"; "
    CHURN
    "    var got := await C.Inner(i); "
    "    return built.Length + got - i; "    // == built.Length, if `got` is right
    "  end Work; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + await C.Work(i); "
    "    end; "
    "    return total; "                     // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (D) EXACTLY-ONCE ORACLE, async — a BORROW local bound from a live object's
// string field. 20 retain/release cycles against the SAME heap string, then the
// field is read again. Over-release => heap-use-after-free at that read;
// under-release => the string outlives the Holder's disposal. §28.3 / §28.9.
// =====================================================================
const char* kAsyncBorrowLocalExactlyOnce =
    HOLDER_DECL
    "class C "
    "  private static async method Work(h: Holder, i: int): Task<int> "
    "    var borrowed := h.Name; "
    CHURN
    "    return borrowed.Length; "
    "  end Work; "
    "  public static async method Main(): Task<int> "
    HOLDER_INIT
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + await C.Work(h, i); "
    "    end; "
    "    return total + h.Name.Length; "     // 20*4 + 4 = 84
    "  end Main; "
    "end C;";

// =====================================================================
// (E) THE USE-AFTER-FREE DIRECTION, async — the callee ASSIGNS to its `string`
// PARAMETER, which the pin turns into a drop-on-overwrite release of a
// reference the CALLER owns (§28.1). The co-gated bind-time retain is what
// keeps the caller's string alive across 10 round trips; the caller reads it
// again after churn each time.
//
// Still LEAKS one reference per call — #2696's documented, deliberate imbalance
// ("No scope-end release is registered for the parameter slot, so its final
// value leaks"), unchanged here; the leak SHRINKS from 60 to 20 allocations
// because the body's own locals are now released. What the assertion pins is
// the pair: a leak IS reported and specifically NOT a use-after-free.
// (Removing the co-gated retain makes this case report heap-use-after-free.)
// =====================================================================
const char* kAsyncAssignedStringParam =
    HOLDER_DECL
    "class C "
    "  private static async method Take(s: string, i: int): Task<int> "
    "    s := s + \"-x\"; "
    CHURN
    "    return s.Length; "
    "  end Take; "
    "  public static async method Main(): Task<int> "
    HOLDER_INIT
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + await C.Take(h.Name, i); "
    "      var churn2 := $\"more-churn-{i}-{i}-{i}\"; "
    "      if churn2.Length < 0 then return 98; end; "
    "      total := total + h.Name.Length; "
    "    end; "
    "    return total; "                     // 10*6 + 10*4 = 100
    "  end Main; "
    "end C;";

// =====================================================================
// (F) #2764's ALIAS RELAXATION, live in an async body for the first time. Its
// author states the change is inert in an async body precisely because of the
// gate this fix flips, so the interaction is exercised here: each link mints its
// own +1 (armStringLocalRefcount retains every member of stringBorrowAll_) and
// the walker consults that very set, so all three slots release exactly once.
// All three are read. §4.6 / §28.3.
// =====================================================================
const char* kAsyncAliasChain =
    "module M; class C "
    "  private static async method Work(i: int): Task<int> "
    "    var built := $\"al-{i}\"; "
    "    var other := built; "
    "    var third := other; "
    CHURN
    "    return built.Length + other.Length + third.Length; "
    "  end Work; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + await C.Work(i); "
    "    end; "
    "    return total; "                     // 3 * 41 = 123
    "  end Main; "
    "end C;";

// =====================================================================
// (G) EARLY RETURN and THROW edges. `built` is discharged by emitAllDisposals on
// the explicit return, by emitDisposals on the fall-off, and by
// emitUnwindDisposals on the throw edge — exactly one of the three runs per
// activation. §28.3 "Exception-path discharge" / §30.9.
// =====================================================================
const char* kAsyncEarlyReturnAndThrow =
    "module M; class C "
    "  private static async method Work(i: int): Task<int> "
    "    var built := $\"ep-{i}\"; "
    CHURN
    "    if i mod 3 = 0 then return built.Length; end; "
    "    if i mod 3 = 1 then throw new InvalidOperationException(\"boom\"); end; "
    "    var tail := $\"tl-{i}\"; "
    "    return built.Length + tail.Length; "
    "  end Work; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      try "
    "        total := total + await C.Work(i); "
    "      catch InvalidOperationException => total := total + 1; "
    "      end; "
    "    end; "
    "    return total; "                     // 27 + 7 + 2*32 = 98
    "  end Main; "
    "end C;";

// =====================================================================
// (H) SEVERAL SPECIALIZATIONS of the SAME generic body, plus a generic METHOD on
// a non-generic class. Each instantiation is its own llvm::Function and must arm
// independently — one shared pin would leave the others inert. §14.1.
// =====================================================================
const char* kGenericSeveralInstantiations =
    "module M; class Box<T> "
    "  public static method Copy(i: int): int "
    "    var built := $\"gc-{i}\"; "
    CHURN
    "    return built.Length; "
    "  end Copy; "
    "end Box; "
    "class Plain "
    "  public static method Mix<U>(u: U, i: int): int "
    "    var built := $\"gm-{i}\"; "
    "    var alias := built; "
    CHURN
    "    return built.Length + alias.Length; "
    "  end Mix; "
    "end Plain; "
    "class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + Box<string>.Copy(i); "
    "      total := total + Box<int>.Copy(i); "
    "      total := total + Plain.Mix<int>(i, i); "
    "    end; "
    "    return total; "                     // 41 + 41 + 2*41 = 164
    "  end Main; "
    "end C;";

// =====================================================================
// (J) THE USE-AFTER-FREE DIRECTION, generic specialization — the (E) shape with
// the callee monomorphized. bindParameters binds the parameter ONCE in the
// specialization's own entry block, so the co-gated retain sits exactly where a
// plain method's does. Same documented per-call leak; same assertion pair.
// =====================================================================
const char* kGenericAssignedStringParam =
    HOLDER_DECL
    "class Box<T> "
    "  public static method Take(s: string, i: int): int "
    "    s := s + \"-x\"; "
    CHURN
    "    return s.Length; "
    "  end Take; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    HOLDER_INIT
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + Box<string>.Take(h.Name, i); "
    "      var churn2 := $\"more-churn-{i}-{i}-{i}\"; "
    "      if churn2.Length < 0 then return 98; end; "
    "      total := total + h.Name.Length; "
    "    end; "
    "    return total; "                     // 10*6 + 10*4 = 100
    "  end Main; "
    "end C;";

// =====================================================================
// (K) EXACTLY-ONCE ORACLE, generic specialization — the (D) shape.
// =====================================================================
const char* kGenericBorrowLocalExactlyOnce =
    HOLDER_DECL
    "class Box<T> "
    "  public static method Peek(h: Holder, i: int): int "
    "    var borrowed := h.Name; "
    CHURN
    "    return borrowed.Length; "
    "  end Peek; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    HOLDER_INIT
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + Box<string>.Peek(h, i); "
    "    end; "
    "    return total + h.Name.Length; "     // 20*4 + 4 = 84
    "  end Main; "
    "end C;";

// =====================================================================
// (L) THE SAVE/RESTORE GUARD. A lazy specialization is built MID-EMISSION of the
// outer body that first calls it. The outer body has its own armed string locals
// declared BOTH BEFORE and AFTER the triggering call site: if
// emitSpecializedMethod left the string sets / fn token pointing at the
// specialization, every local after the call site would silently lose its
// release. Pre-fix this program leaked exactly the specialization's own local
// (20 allocations, 10 iterations x 1 string x 2) — the outer's two were already
// released — so a broken save/restore shows up as that same 20 coming back for
// `after` instead. §14.1 / §28.9.
// =====================================================================
const char* kSpecializationBuiltMidOuterBody =
    "module M; class Box<T> "
    "  public static method Copy(i: int): int "
    "    var inner := $\"in-{i}\"; "
    "    return inner.Length; "
    "  end Copy; "
    "end Box; "
    "class C "
    "  private static method Outer(i: int): int "
    "    var before := $\"be-{i}\"; "
    "    var mid := Box<string>.Copy(i); "
    "    var after := $\"af-{i}\"; "
    CHURN
    "    return before.Length + mid + after.Length; "
    "  end Outer; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + C.Outer(i); "
    "    end; "
    "    return total; "                     // 3 * 41 = 123
    "  end Main; "
    "end C;";

// =====================================================================
// (M) an ASYNC ITERATOR body. Its resume runs once per MoveNextAsync and it has
// a separate abandonment path (Dispose while suspended at a yield), so #2807
// filed it as a follow-up rather than arming it here; #2817 then armed it (the
// bind and the scope-end release land in consecutive advances, one each, and the
// abandonment path is covered by the yield handler's cleanup snapshot). §30.7 /
// §29.7.
// =====================================================================
const char* kAsyncIteratorBody =
    "module M; class C "
    "  private static async method Seq(n: int): IAsyncEnumerable<int> "
    "    for var i in 1..n do "
    "      var built := $\"it-{i}\"; "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      if churn.Length < 0 then yield return 99; end; "
    "      yield return built.Length; "
    "    end; "
    "  end Seq; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    await for var v in C.Seq(20) do "
    "      total := total + v; "
    "    end; "
    "    return total; "                     // 91
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------
// (A) the issue's repro A — a plain async body.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_body_producer_local_is_released_spec_30_1) {
    JitResult r = runAotChecked(kAsyncProducerLocals);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 1904 B / 80 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_body_producer_local_plain_path_value_invariant_spec_30_1) {
    // NOT a #2807 oracle — a leak is invisible without a sanitizer, so this
    // passes with and without the fix. It pins that nothing is freed early.
    JitResult r = runAot(kAsyncProducerLocals);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (B) the issue's repro B — a generic-specialization body.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           generic_specialization_producer_local_is_released_spec_14_1) {
    JitResult r = runAotChecked(kGenericSpecProducerLocal);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 1904 B / 80 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           generic_specialization_plain_path_value_invariant_spec_14_1) {
    JitResult r = runAot(kGenericSpecProducerLocal);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (C) the local lives across an `await` — the frame-spill dimension.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_local_live_across_await_is_released_once_spec_30_2) {
    JitResult r = runAotChecked(kAsyncLocalAcrossAwait);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 1904 B / 80 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_local_live_across_await_plain_path_value_spec_30_2) {
    // The strong direction: were the release to fire on the SUSPENDING
    // activation too, the post-resume read would be of freed memory.
    JitResult r = runAot(kAsyncLocalAcrossAwait);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (D) / (K) EXACTLY-ONCE oracles — 20 retain/release cycles against a live
// object's field, then the field is read again.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_borrow_local_releases_exactly_once_spec_28_3) {
    JitResult r = runAotChecked(kAsyncBorrowLocalExactlyOnce);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 1153 B / 40 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_borrow_local_field_survives_every_cycle_spec_28_3) {
    JitResult r = runAot(kAsyncBorrowLocalExactlyOnce);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           generic_borrow_local_releases_exactly_once_spec_28_3) {
    JitResult r = runAotChecked(kGenericBorrowLocalExactlyOnce);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 1153 B / 40 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           generic_borrow_local_field_survives_every_cycle_spec_28_3) {
    JitResult r = runAot(kGenericBorrowLocalExactlyOnce);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 84);
}

// ---------------------------------------------------------------------
// (E) / (J) the use-after-free direction — an ASSIGNED-TO `string` parameter.
// (E) still leaks the parameter slot's final value (#2696's documented trade);
// (J) no longer does (#5253). Neither may free the CALLER's reference. Removing the co-gated retain
// turns both of these into heap-use-after-free (verified).
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_assigned_string_param_does_not_free_caller_spec_28_1) {
    JitResult r = runAotChecked(kAsyncAssignedStringParam);
    if (wasSkipped(r)) return;
    expectLeakButNoMemoryError(r);         // 1446 B / 60 -> 390 B / 20
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_assigned_string_param_caller_value_survives_spec_28_1) {
    JitResult r = runAot(kAsyncAssignedStringParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// fix #5253 (§28.9, v0.313): the specialization's assigned parameter never
// escapes, so its final value is now released at scope end, and #2696's
// per-call leak is gone for (J). The run must therefore be clean under ASan
// AND LSan: a leak OR a use-after-free fails it. (E), the async body, still
// keeps the documented leak; the scope-end release is not armed there.
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           generic_assigned_string_param_does_not_free_caller_spec_28_1) {
    JitResult r = runAotChecked(kGenericAssignedStringParam);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 953 B / 40 -> 390 B / 20 -> 0
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           generic_assigned_string_param_caller_value_survives_spec_28_1) {
    JitResult r = runAot(kGenericAssignedStringParam);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}

// ---------------------------------------------------------------------
// (F) #2764's alias relaxation, live in an async body for the first time.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_alias_chain_releases_every_link_spec_4_6) {
    JitResult r = runAotChecked(kAsyncAliasChain);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 934 B / 40 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 3 * kLenSum10);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_alias_chain_plain_path_value_invariant_spec_4_6) {
    JitResult r = runAot(kAsyncAliasChain);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3 * kLenSum10);
}

// ---------------------------------------------------------------------
// (G) early return and throw edges.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_early_return_and_throw_edges_release_once_spec_28_3) {
    JitResult r = runAotChecked(kAsyncEarlyReturnAndThrow);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 2167 B / 94 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 98);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_early_return_and_throw_plain_path_value_spec_28_3) {
    JitResult r = runAot(kAsyncEarlyReturnAndThrow);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 98);
}

// ---------------------------------------------------------------------
// (H) several instantiations of one generic body.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           every_instantiation_arms_independently_spec_14_1) {
    JitResult r = runAotChecked(kGenericSeveralInstantiations);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 2802 B / 120 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 4 * kLenSum10);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           every_instantiation_plain_path_value_invariant_spec_14_1) {
    JitResult r = runAot(kGenericSeveralInstantiations);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4 * kLenSum10);
}

// ---------------------------------------------------------------------
// (L) the specialization is built mid-emission of the outer body.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           outer_body_keeps_its_own_string_state_spec_14_1) {
    JitResult r = runAotChecked(kSpecializationBuiltMidOuterBody);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 371 B / 20 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, 3 * kLenSum10);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           outer_body_plain_path_value_invariant_spec_14_1) {
    JitResult r = runAot(kSpecializationBuiltMidOuterBody);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3 * kLenSum10);
}

// ---------------------------------------------------------------------
// (M) WAS the deliberate exclusion, pinned as a characterization guard: an async
// ITERATOR body had to keep LEAKING rather than turn into a use-after-free.
//
// fix #2817 (v0.240) REMOVED that exclusion — emitAsyncIteratorMethod now runs
// collectStringReleaseEligible for its resume fn and co-gates the parameter
// retain, so this program is clean (1904 B / 80 allocations -> none), and its
// abandonment path is covered by the yield handler's smIterCleanupAtState_
// snapshot. The characterization is updated rather than deleted: the case still
// earns its place as the async-iterator arm of the exactly-once oracle, and the
// assertion direction is now the stronger one (`r.ran` fails on a leak AND on a
// use-after-free). #2817's own file carries the full five-body-kind grid.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_iterator_body_locals_are_released_after_2817_spec_30_7) {
    JitResult r = runAotChecked(kAsyncIteratorBody);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);              // 1904 B / 80 allocations pre-#2817
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_async_generic_string_local_release_2807,
           async_iterator_body_value_is_correct_spec_30_7) {
    JitResult r = runAot(kAsyncIteratorBody);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}
