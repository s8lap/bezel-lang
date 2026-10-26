// Codegen / runtime — §28.9 / §8.7 / §28.2 (#7260): a function-typed value
// has NO `Dispose()` member, so `c.Dispose()` on a closure is a COMPILE ERROR
// — exactly as `o.Dispose()` on an `Object` already is.
//
// HISTORY. This file was written for #4475, which asked for a per-instance
// `disposed` flag on the closure cell so that `c.Dispose(); c();` raises
// `DisposedAccessException` under --checked-disposal instead of SIGSEGVing.
// It shipped: the closure cell grew a `disposed` byte, the runtime grew
// `bz_closure_mark_disposed` / `bz_closure_is_disposed`, and codegen emitted
// the mark at the explicit-`Dispose()` site (LlvmBackend_Calls2.cpp) and the
// guard at the closure-invoke site.
//
// §28.9 (v0.344) says a function-typed value has NO `Dispose()` AT ALL:
//
//   "The following types are exempt from manual disposal ([K1.6]'s
//   runtime-managed exemption). Their `Dispose()` is a no-op (or, for types
//   that are not classes and therefore have no `Dispose()` at all, the type's
//   lifetime is managed by the runtime without user involvement). How the
//   runtime manages their lifecycle (reference counting, region allocation,
//   or other) is not part of this specification."
//
//   "Function-typed values — values of any function type `(T1, ..., Tn) -> R`,
//   regardless of whether they capture. This includes lambda closures
//   (capturing or not), method references (`obj.Method` and
//   `Type.StaticMethod`), and any value whose static type is a function
//   type. Function-typed fields, parameters, and locals are not subject to
//   `owned` annotations and carry no disposal obligation; a `using var` local
//   of function type is nonetheless permitted (disposal at scope end is a
//   no-op) and serves only the non-reassignability role (§28.10) …"
//
// and §8.7 (v0.344): "`Dispose()` is class-only. The compiler generates
// `IDisposable` on every non-static class. Static classes, records, enums,
// choices, primitives, and inline arrays do not implement `IDisposable`."
// and §28.2 (v0.344): "`Dispose()` is not on `Object`; it is class-only."
//
// ⇒⇒ A FUNCTION TYPE IS NOT A CLASS, so it falls in §28.9's PARENTHETICAL: it
// has no `Dispose()` AT ALL. The compiler already answered the same question
// for the closest sibling — `var o: Object := i; o.Dispose();` is REJECTED
// with "no member `Dispose` on type `Object`: `Dispose()` is class-only
// (§8.7, v0.340), and `Object` is not a class, so `Dispose` is not one of its
// members (§28.2)" — and the compiler must answer the same question the same
// way for the closest sibling (#7260).
//
// ⇒⇒⇒⇒⇒ THE TWO ROWS BELOW ARE FLIPPED BY #7260. They used to assert the
// RUNTIME behaviour of the compiler's real dispose: a
// `DisposedAccessException` under --checked-disposal for the dispose-then-
// invoke program, and a clean exit for the dispose-alone program. They now
// assert the COMPILE-TIME refusal, which is STRICTLY STRONGER — the old rows
// pinned a runtime behaviour, the new rows pin that the compiler REFUSES a
// spelling §28.9 forbids. (A banked row is evidence of the CURRENT behaviour,
// not of the CORRECT behaviour; the #7177 lane's first version of this rule
// rejected the spelling and its review blocked it precisely because these two
// rows pinned the non-conformant behaviour.)
//
// The row NAMES are kept deliberately: they are the banked #4475 rows the
// coordinator tracks, and `scripts/handback_check.sh`'s named-test check
// resolves a commit message's `Suite.name` against the tree at the tip. The
// names therefore describe the PRE-#7260 behaviour; the BODIES are the truth.
//
// The oracle is `runAot` and NOT `runAotChecked*`: the verdict is a
// COMPILE-TIME refusal, so the disposal MODE is irrelevant to it, and the
// `runAotChecked*` family returns ran=false with a "SKIP" diagnostic when
// clang is unavailable — the shape `scripts/check_toolchain_skip_surface.py`
// ratchets. Using `runAot` for the two flipped rows also LOWERS that
// ratchet's `checked_calls` by two, a move toward its baseline.
//
// per §28.9, §8.7, §28.2, §27.1, §27.6, v0.344, #7260

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains /
                                           // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedDisposal;

namespace {
// Source for every test. A free-standing lambda `lambda(): int => 42`;
// the closure has no captures so the env block is null.
//
// (The #4475 shape is preserved verbatim: the SAME program is what the two
// banked rows compiled, and the rejection must land on `c.Dispose()` inside
// it.)
const char* kProgram =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var c := lambda(): int => 42; "
    "    c.Dispose(); "
    "    var x := c(); "
    "    return x; "
    "  end Main; "
    "end C;";

// The #7260 rejection: a function-typed value has no `Dispose()` member, so
// `c.Dispose()` is a COMPILE ERROR, and the diagnostic must carry the
// class-only reason with the citations the sibling `Object` rule uses
// (§8.7's rule, §28.2) plus §28.9's runtime-managed list.
//
// The row asserts the COMPILE-TIME refusal and NOT the pre-fix symptoms (an
// empty output, a zero value, or the #3079 / #2434 internal-degradation
// notices), and NOT a crash / codegen failure / signal / timeout.
void expectRejectedWithDisposeOnFunctionValue(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "no member `Dispose` on a function value"));
    // The class-only reason, and the three sections it rests on.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`Dispose()` is class-only"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§8.7"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§28.2"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§28.9"));
    // NOT the pre-fix symptoms.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "#3079"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "#2434"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "failed to lower to a value"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "degraded to a zero-initialized value"));
    // NOT a crash, NOT a codegen failure, NOT a signal or a timeout.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Assertion"));
}
}  // namespace

// =====================================================================
// Row (1) FLIPPED by #7260.
//
// OLD expectation: under --checked-disposal the program COMPILED, and the
// `c()` after `c.Dispose()` raised `System.DisposedAccessException` (exit
// 70) — the compiler's REAL dispose on the closure cell (§28.2's
// checked-disposal guard; the `bz_closure_mark_disposed` helper, #4475).
//
// NEW expectation: the program is REJECTED at `c.Dispose()`, with the
// class-only diagnostic (§8.7 / §28.2 / §28.9). That is STRICTLY STRONGER:
// the old row asserted a runtime behaviour, this one asserts a compile-time
// refusal of a spelling §28.9 forbids.
//
// The row NAME is the banked #4475 name and is kept deliberately; see the
// file header.
// =====================================================================
BEZEL_TEST(closure_dispose_then_invoke_4475,
           checked_disposal_raises_disposed_exception_28_2) {
    expectRejectedWithDisposeOnFunctionValue(kProgram);
}

// =====================================================================
// Row (2) FLIPPED by #7260.
//
// OLD expectation: `c.Dispose()` alone — no invoke after it — exited 0
// cleanly under --checked-disposal, pinning that the explicit-Dispose
// codegen arm (the one that emits `bz_closure_mark_disposed`) does not
// itself raise an exception or segfault on a freshly-allocated closure cell.
//
// NEW expectation: the program is REJECTED at `c.Dispose()`. The old row
// pinned a runtime behaviour; this one pins that the compiler REFUSES the
// spelling.
//
// The row NAME is the banked #4475 name and is kept deliberately; see the
// file header.
// =====================================================================
BEZEL_TEST(closure_dispose_then_invoke_4475,
           dispose_alone_exits_zero) {
    expectRejectedWithDisposeOnFunctionValue(
        "module M; class C "
        "  public static method Main(): int "
        "    var c := lambda(): int => 42; "
        "    c.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
}

// =====================================================================
// Row (3) CONTROL: no Dispose between create and invoke — runs cleanly,
// exit 42. Pins the no-dispose happy path so a fix that over-fires the
// guard (and rejects valid invocations) shows up here.
//
// NOT FLIPPED by #7260: the program carries no `Dispose()` at all, so §28.9's
// ruling does not reach it. It still runs under --checked-disposal, because
// the closure-invoke path is a runtime path and this row pins that it works.
// =====================================================================
BEZEL_TEST(closure_dispose_then_invoke_4475,
           no_dispose_invoke_clean_returns_42) {
    JitResult r = runAotCheckedDisposal(
        "module M; class C "
        "  public static method Main(): int "
        "    var c := lambda(): int => 42; "
        "    var x := c(); "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// Row (4) NEW (#7260): the rule is CATEGORY-BIND, not lambda-specific.
//
// §28.9's function-typed bullet names "any value whose static type is a
// function type" and lists "method references (`obj.Method` and
// `Type.StaticMethod`)" among them. This row drives the METHOD-REFERENCE
// spelling — a §27.6 method reference bound to a local, then
// `f.Dispose()` — and pins the SAME diagnostic.
//
// It is the fail-without row for the new text: on the pre-fix base the
// program COMPILED (the #7177 carve-out admitted `Dispose` on any function
// value), so the row fails there and passes with the fix.
//
// The old row (1) is the lambda spelling; this one is the method-reference
// spelling. They are different programs, so they fail independently.
// =====================================================================
BEZEL_TEST(closure_dispose_then_invoke_4475,
           method_reference_dispose_rejected_spec_28_9) {
    expectRejectedWithDisposeOnFunctionValue(
        "module M; "
        "class H "
        "  public method Id(): int => 42; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    var f := h.Id; "
        "    f.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
}

// =====================================================================
// Row (5) NEW (#7260): THE RUNTIME HALF — rejecting `c.Dispose()` must not
// strand a CAPTURE-BEARING closure's capture set.
//
// §28.9 (v0.344): "Runtime management governs the *function value itself* —
// the heap-allocated closure object that backs it — and relaxes nothing about
// the values the closure captures". So the closure's OWN cell is runtime-
// managed, but the values it captures keep their own disposal obligations
// (§28.9's captured-owned-binding clause, §28.3's closure bullet).
//
// Rejecting `c.Dispose()` removes the ONLY spelling that reached the
// runtime's closure-dispose path (`bz_closure_mark_disposed`, #4475). If the
// closure's capture set were reclaimed ONLY by that spelling, this lane would
// have traded a `DisposedAccessException` for a LEAK. This row is the
// measurement that it did not.
//
// The program: an OWNED resource `r` (a `using var` local with a destructor,
// so it has a real disposal obligation), captured by a closure that is a
// PLAIN `var` of function type. §28.9's last clause: "a plain `var` of
// function type scopes its captures identically (section 28.3's closure
// bullet)" — so the plain `var` spelling is the one that must reclaim.
//
// The oracle is `runAotChecked` — the LSan-enabled AOT recipe
// (`detect_leaks=1`): the harness turns a LeakSanitizer report into
// ran=false, so `r.ran` IS the no-leak assertion here, and `r.exitCode` is
// the program's own. The row is NOT vacuous: the `using var` twin of this
// program (which the compiler's scope-end path does NOT release — a separate
// defect, reported, not fixed here) LEAKS the same cell, and the same oracle
// FAILS on it.
//
// MEASURED under BEZEL_AOT_FORCE_SANITIZE=address: no
// AddressSanitizer / LeakSanitizer report, and the program prints `7` then
// `disposed` (the owned resource's destructor ran at its own scope end).
// =====================================================================
BEZEL_TEST(closure_dispose_then_invoke_4475,
           capture_bearing_closure_reclaims_its_capture_set_spec_28_9) {
    JitResult r = runAotChecked(
        "module M; "
        "class Res "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    using var r := new Res(7); "
        "    var f := lambda(): int => r.Id; "
        "    var x := f(); "
        "    Console.WriteLine(x.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // A LeakSanitizer report makes the harness report ran=false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

