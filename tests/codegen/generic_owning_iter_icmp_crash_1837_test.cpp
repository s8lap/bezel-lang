// Codegen / crash — a GENERIC owning-enumerable iterator
// (`IOwningEnumerable<T> where T: class`) must lower without tripping the
// `llvm::ICmpInst::AssertOK` "Both operands to ICmp instruction are not of the
// same type!" assertion. Issue #1837 (split off from #1645). Spec: stdlib §4.13
// (owning-enumeration interfaces constrain `where T: class`), §29.1 v0.196
// (IOwningEnumerable / TakeCurrent / enumerator-owned current_val disposal).
//
// THE BUG (#1837). emitIteratorMethod's `disposeYieldedVal` (the Dispose-side
// cleanup of the enumerator-owned `current_val`, LlvmBackend_Iterators.cpp)
// loads current_val as `elemTy` and compares it against a null POINTER —
// requiring `elemTy` to be a pointer. For an IOwningEnumerable<T> iterator
// emitted as its UNSPECIALIZED generic template (T is an unresolved type
// parameter, so `lowerTypeRef` falls to the i32 value-kind placeholder), that
// branch still ran (`isOwningIter` is true) with a NON-pointer `elemTy`, so the
// raw `i32 == ptr null` ICmp aborted bezelc in codegen (exit 134) — even for an
// empty `yield break;` body. The generic template is emitted for IR-completeness
// (§14.1 monomorphization) whether or not the method is ever instantiated, so the
// crash fired on any program merely DECLARING such an iterator. #1645 had already
// fixed the value-element trigger (`IOwningEnumerable<int>` now a clean stdlib
// §4.13 sema reject); a class-constrained generic T is NOT a §4.13 violation, so
// it correctly reached — and crashed in — codegen.
//
// THE FIX. Guard `disposeYieldedVal` on `elemTy->isPointerTy()` (return early
// otherwise), mirroring the identical `isOwningIter && elemTy->isPointerTy()`
// guards already present at the TakeCurrent (§29.1) and MoveNext-exhaustion
// sites. The unspecialized template is never called; each real call site emits a
// monomorphized specialization where the class-constrained T is a concrete
// reference type (elemTy is `ptr`), so the full owned-current_val disposal still
// runs there. `elemIsOwnedClass` always implies a class/interface element ⇒ a
// pointer elemTy, so the guard never elides a real IEnumerable<owned T> disposal.
//
// OBSERVABILITY. The compile is fork-isolated in the AOT harness, so WITHOUT the
// fix the SIGABRT surfaces as `ran == false` (the whole program failed to
// compile); WITH the fix the program compiles, runs, and — for the instantiated
// case — disposes every yielded element exactly once with no leaked iterator
// frame (runAotChecked's ASan/LSan). Both the empty-body template (the exact
// issue repro) and a real class instantiation are pinned.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// The exact #1837 repro: a generic method returning IOwningEnumerable<T> where
// T: class with an empty `yield break;` body. The generic template is emitted
// (never instantiated here — Gen is not called), and WITHOUT the fix bezelc
// aborts in codegen (ICmp i32-vs-ptr) before producing an object. WITH the fix it
// compiles; Main returns 0.
const char* kEmptyBodyTemplate =
    "module M; "
    "class Foo "
    "  public method Gen<T>(): IOwningEnumerable<T> where T: class "
    "    yield break; "
    "  end Gen; "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end Foo;";

// A generic owning iterator with a real yield body (a `() -> owned T` factory so
// the yielded value is a fresh, cleanly-transferred allocation — not an owned
// param, which is a separate, unrelated owned-capture concern). Declared but not
// called: still emits the template that crashed without the fix.
const char* kYieldBodyTemplate =
    "module M; "
    "class Foo "
    "  public method Gen<T>(make: () -> owned T): IOwningEnumerable<T> "
    "                                                    where T: class "
    "    yield return make(); "
    "  end Gen; "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end Foo;";

// The generic owning iterator INSTANTIATED at a concrete class R and consumed via
// the §29.1 manual owning protocol (source / GetOwningEnumerator / MoveNext /
// TakeCurrent). Each yielded R is adopted by a `using var` and disposed exactly
// once (destructor prints "D<Id>"); the two enumerator frames are freed by their
// `using var`s. Requires the generic template to compile (crash pin) AND the R
// specialization to run + dispose correctly. Sum of Ids 1+2+3 = 6.
const char* kInstantiatedManualProtocol =
    "module M; "
    "class R "
    "  public static _seq: int := 0; "
    "  public Id: int := 0; "
    "  public constructor() R._seq := R._seq + 1; Id := R._seq; end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end R; "
    "class C "
    "  public static method Gen<T>(make: () -> owned T): IOwningEnumerable<T> "
    "                                                    where T: class "
    "    yield return make(); "
    "    yield return make(); "
    "    yield return make(); "
    "  end Gen; "
    "  public static method Main(): int "
    "    using var source := Gen<R>(lambda() => new R()); "
    "    using var en := source.GetOwningEnumerator(); "
    "    var sum := 0; "
    "    while en.MoveNext() do "
    "      using var u := en.TakeCurrent(); "
    "      sum := sum + u.Id; "
    "    end; "
    "    return sum; "   // 1 + 2 + 3 = 6
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #1837 — the empty-body generic owning-iterator template compiles (no ICmp
// codegen assertion). stdlib §4.13 / §29.1 v0.196.
// =====================================================================

BEZEL_TEST(generic_owning_iter_icmp_crash_1837,
           empty_body_template_compiles_spec_4_13) {
    // The exact issue repro. WITHOUT the fix bezelc aborts on
    // llvm::ICmpInst::AssertOK during codegen of Gen's Dispose (i32 current_val
    // vs a null ptr) — the fork-isolated compile fails, so r.ran is false. WITH
    // the fix it compiles and Main returns 0.
    JitResult r = runAot(kEmptyBodyTemplate);
    BEZEL_EXPECT_TRUE(r.ran);          // compiled — no codegen ICmp assertion
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(generic_owning_iter_icmp_crash_1837,
           yield_body_template_compiles_spec_29_1) {
    // Same template crash for a non-empty (`yield return make();`) body.
    JitResult r = runAot(kYieldBodyTemplate);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// #1837 — the generic owning iterator INSTANTIATED at a concrete class runs and
// disposes every yielded element exactly once with no leaked frame (§29.1). This
// also re-pins the template-compile crash (the template is emitted alongside the
// R specialization). runAotChecked = ASan/LSan oracle.
// =====================================================================

BEZEL_TEST(generic_owning_iter_icmp_crash_1837,
           instantiated_at_class_disposes_clean_spec_29_1) {
    JitResult r = runAotChecked(kInstantiatedManualProtocol);
    BEZEL_EXPECT_TRUE(r.ran);            // compiled + LSan-clean (no leaked frame)
    BEZEL_EXPECT_EQ(r.exitCode, 6);      // 1 + 2 + 3
    // Each yielded element disposed exactly once (a leak or double-free would also
    // have flipped r.ran; these pin the exact-once count positively).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D3"), 1);
}
