// Spec-derived regression test for issue #7316 — a `using var` COPY of a
// value-like choice local leaked, while the plain-`var` twin of the SAME
// program was clean.
//
// THE SPEC ANCHOR — §28.10 "`using var`" (v0.344), the runtime-managed
// paragraph, verbatim:
//
//   "`using var` accepts a value-type or runtime-managed right-hand side. For
//    ordinary value types and runtime-managed types, the disposal at scope end
//    is a no-op (these types do not require disposal per §28.9), and the only
//    effect is the reassignability restriction."
//
// => THREE requirements, all pinned here:
//
//   (1) "the disposal at scope end is a no-op" is about the DISPOSAL
//       OBLIGATION: a value-like choice is an ordinary copyable VALUE type
//       (§35.4), it has no `Dispose()` to call. It is NOT a licence to skip
//       releasing the runtime's own reference — the +1 the §35.3 construction
//       minted on the payload string, which the plain-`var` twin owns and
//       releases at scope end. (Rows 1-3.)
//   (2) "and the only effect is the reassignability restriction" — the
//       `using var` spelling of a runtime-managed value must have the SAME
//       runtime lifetime as the plain-`var` spelling. The COPY spelling is a
//       copy of a value type (§3.6: a value type is copied on assignment), so
//       the copy owns its OWN reference to the payload string and must release
//       it at scope end, exactly as its plain-`var` twin does. One released the
//       payload and the other did not: a direct contradiction of that clause.
//       (Rows 1-3, and the `var` control row 5.)
//   (3) The reassignability restriction itself must SURVIVE the fix — a
//       `using var` is a scope pin (axiom A5), and the §28.10 lint that
//       reports the vacuous spelling must still fire. (Rows 10-11.)
//
// §28.9 "Runtime-managed types" (v0.344) supplies the carve the fix rests on:
// the runtime manages the payload string's lifecycle, and §28.9's rule for a
// runtime-managed value is that the COMPILER does not warn about undisposed
// runtime-managed values — there is no disposal OBLIGATION on this value, so
// the only thing that can be wrong here is the RUNTIME LIFETIME. §35.4
// supplies the classification the fix keys on: a value-like choice is "an
// ordinary copyable value type", and its payload's ownership is "defined by
// the active case".
//
// THE DEFECT, MEASURED (the base arm, `bezelc --sanitize=address`, LSan with
// `use_stacks=0`):
//
//   var a := Msg.Note("a" + "b"); using var b := a;  ->  35 byte(s) leaked in
//                                                      2 allocations
//   var a := Msg.Note("a" + "b"); var         b := a;  ->  clean
//
// ⇒ THE TWO-SPELLING MATRIX, with the `using var` arm the odd one out. The
// ASYMMETRY IS EXACTLY #7305'S, IN A DIFFERENT PLACE: #7305 fixed the
// CONSTRUCTION path of the same §28.10 clause, and the COPY path — a
// different predicate — still refused the `using var` spelling.
//
// THE SITE. `Emitter::valueLikeChoiceLocalIsCopy` (src/codegen/
// LlvmBackend_Setup.cpp) — the COPY classification — refused `v.isUsing`, so
// the `using var` spelling of a copy was never classified as a copy, never
// became a candidate, and never entered
// `valueLikeChoiceStrReleaseEligible_`. Two consequences followed, and the
// relocation table of the base arm shows BOTH: the copy was never armed (no
// retain at bind, no scope-end release for the copy's own reference), and the
// SOURCE was dropped from the armed set too (the escape walk's
// `choiceOwnerAliases` relaxation names an armed copy, and the copy was not
// one), so the source's own +1 was never discharged either.
//
// ── THE ORACLES (and why they differ per row) ─────────────────────────────
//
// ⚠ EVERY LEAK ROW IS LOOPED, AND THAT IS LOAD-BEARING. A ONE-SHOT leak
// repro is a VACUOUS ORACLE here: LSan scans the stack for roots by default
// (`use_stacks=1`), and the dead `Main` frame still holds the payload
// pointer, so the SINGLE leaked allocation is read as reachable and LSan
// reports NOTHING. MEASURED on the base arm: the one-shot copy shape leaks
// 35 byte(s) in 2 allocations under `LSAN_OPTIONS=use_stacks=0` and reports
// NOTHING under the harness's default options — the one-shot row is GREEN on
// the base arm. The 101-iteration rows are RED on the base arm (the harness
// reports "assertion failed: r.ran"), because the earlier iterations'
// pointers are gone from the stack by the time LSan runs. Every leak row
// below therefore loops.
//
// ⚠ LEAK ROWS USE THE CHECKED ORACLE. `runAot` pins `detect_leaks=0`
// (tests/support/aot_support.hpp), so a plain `runAot` row is VACUOUS for a
// leak — the program can leak and the row stays green. `runAotChecked` sets
// `detect_leaks=1` and reports a leak as a sanitizer failure, so
// `r.ran == false` is the leak verdict. The toolchain skip is asserted
// LOUDLY (`checkedToolchainSkipped`), never absorbed by a `return` — a box
// whose clang cannot link ASan has not tested the memory claim, and a silent
// skip is indistinguishable from a pass.
//
// ⚠ EVERY ROW ASSERTS THE PROGRAM'S PRINTED OUTPUT AND EXIT CODE. A leak row
// that only checked "no sanitizer report" would still pass on a program that
// printed the wrong text, and would pass on a compile failure.
//
// ⚠ EVERY ROW ASSERTS THE NO-LEAK SIDE EXPLICITLY (`r.ran`), so a row that
// only asserted "the leak is gone" without pinning the program's behaviour
// could not be satisfied by a program that never ran.
//
// ROWS (each named below):
//
//   1. LEAK          the `using var` COPY of a value-like choice payload,
//                    101 iterations -> no leak (RED before)
//   2. LEAK + UAF    the same with an ALIAS of the copy, 101 iterations -> no
//                    leak AND no over-release (RED before)
//   3. LEAK          a CHAIN of two `using var` copies, 101 iterations ->
//                    no leak (RED before)
//   4. CONTROL       the ONE-SHOT copy shape — the issue's literal repro. NOT
//                    a leak oracle (see the stack-root note above): it pins
//                    that the shape COMPILES, RUNS, exits 0 and prints "done"
//   5. CONTROL       the plain-`var` copy twin of row 1    (green both)
//   6. CONTROL       #7305's shape — the `using var` CONSTRUCTION (green
//                    both)
//   7. CONTROL       a COPY of a choice with an INT payload (green both)
//   8. CONTROL       a COPY of a CLASS-typed (borrow-bearing) choice (green
//                    both)
//   9. CONTROL       the `await using var` spelling — §28.12 owns it, so it
//                    must be UNCHANGED by this fix (green both; the leak it
//                    carries is reported to the coordinator, not pinned here)
//  10. DIAGNOSTIC    the §28.10 "vacuous" warning STILL fires (green both)
//  11. DIAGNOSTIC    the §28.10 pin STILL rejects a reassignment (green both)

#include <cstdio>
#include <string>

#include "support/Diagnostic.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ── the §28.10 "vacuous" warning needle ────────────────────────────────────
// The exact wording of the ownership pass's lint, stable fragments only:
// "`using var` `c` is vacuous" + "no disposal obligation (§28.9)".
const char* kVacuous = "is vacuous";
const char* kVacuousObligation = "no disposal obligation";

// ── the §28.10 non-reassignability needle ──────────────────────────────────
const char* kReassign = "cannot reassign `using var` local";

// ── the programs ────────────────────────────────────────────────────────────
//
// `Msg` is the canonical §35.4 VALUE-LIKE choice: every payload is a value
// (`string` is runtime-managed, §28.9), so the choice is an ordinary copyable
// value type and the §35.3 construction mints a +1 on the payload string.
// The `+` concatenation is the +1 PRODUCER (a literal payload would be an
// immortal sentinel, on which retain and release are no-ops — a vacuous row).
//
// The COPY spelling under test: `var a := Msg.Note("a" + "b"); using var b := a;`
// `b` is a copy of a value type (§3.6), so `b` owns its OWN reference to the
// payload string and releases it at scope end, exactly as the plain-`var`
// twin `var b := a;` does.
//
// `0..100` is INCLUSIVE, so every loop body runs 101 times.

// (1) THE DEFECT, looped. The `using var` spelling of a COPY of a value-like
// choice. RED before the fix: the base arm leaked 3535 byte(s) in 202
// allocations, so `runAotChecked` reported a sanitizer failure and `r.ran`
// was false.
const char* kUsingVarChoiceCopyLoop =
    "module M7316a; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    var n := 0; "
    "    for var i in 0..100 do "
    "      var a := Msg.Note(\"a\" + \"b\"); "
    "      using var b := a; "
    "      n := n + 1; "
    "    end for; "
    "    Console.WriteLine(n.ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (2) THE OVER-RELEASE PROBE, looped. An alias of the `using var` copy. Each
// armed copy takes its own reference at bind and releases it at scope end,
// while the SOURCE owns the construction's +1 and releases it at scope end. A
// fix that released the payload TWICE would drive the refcount negative —
// "refcount over-release (string)", an unconditional abort in
// src/runtime/bezel_runtime_refcounting.cpp — which the ASan arm of this row
// catches as a FAILURE, not a pass. The row also pins that the alias's own
// reference is balanced (an alias WITHOUT its retain would be a
// use-after-free, also caught by ASan).
const char* kUsingVarChoiceCopyAliasedLoop =
    "module M7316b; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    var n := 0; "
    "    for var i in 0..100 do "
    "      var a := Msg.Note(\"a\" + \"b\"); "
    "      using var b := a; "
    "      var d := b; "
    "      n := n + 1; "
    "    end for; "
    "    Console.WriteLine(n.ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (3) A CHAIN of two `using var` copies, looped. `c` copies `b`, which copies
// `a`: every link must take its own reference at bind and drop it at scope
// end, and the SOURCE must still drop the construction's +1. This is the row
// that would catch a fix that armed only the FIRST copy.
const char* kUsingVarChoiceCopyChainLoop =
    "module M7316c; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    var n := 0; "
    "    for var i in 0..100 do "
    "      var a := Msg.Note(\"a\" + \"b\"); "
    "      using var b := a; "
    "      using var c := b; "
    "      n := n + 1; "
    "    end for; "
    "    Console.WriteLine(n.ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (4) CONTROL — the ONE-SHOT copy shape, the issue's literal repro. NOT a
// leak oracle: LSan reads the dead frame's stale pointer as a root (see the
// header note), so this shape is GREEN on the base arm and a leak here would
// be invisible. The row pins what IS observable in one shot — that the shape
// COMPILES, RUNS, exits 0 and prints "done" — so a fix that turned the
// spelling into a compile error, or into a wrong answer, is caught here.
const char* kUsingVarChoiceCopyOnce =
    "module M7316d; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    var a := Msg.Note(\"a\" + \"b\"); "
    "    using var b := a; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (5) CONTROL — the plain-`var` copy twin of row 1. Green on BOTH arms: it is
// the reference behaviour the fix must reproduce, and it proves the fix did
// not change the reference path.
const char* kVarChoiceCopy =
    "module M7316e; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    var a := Msg.Note(\"a\" + \"b\"); "
    "    var b := a; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (6) CONTROL — #7305's shape, the `using var` CONSTRUCTION of the same
// value-like choice. The fix must not disturb it. Green on BOTH arms.
const char* kUsingVarChoiceConstruction =
    "module M7316f; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    using var c := Msg.Note(\"a\" + \"b\"); "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (7) CONTROL — a COPY of a choice whose payload is an INT (a value type with
// no runtime-managed slot). The scope-end walk touches only string-typed
// payload slots, so the fix must not have widened it. Green on BOTH arms.
const char* kUsingVarIntPayloadCopy =
    "module M7316g; "
    "choice Num N(v: int), Z; end Num; "
    "public class Program "
    "  public static method Main(): int "
    "    var a := Num.N(5); "
    "    using var b := a; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (8) CONTROL — a COPY of a CLASS-typed (borrow-bearing) choice. §35.4
// classifies it BorrowBearing, not ValueLike, so the value-like payload path
// does not apply to it at all: the fix must NOT change it. Green on BOTH arms.
const char* kUsingVarClassChoiceCopy =
    "module M7316h; "
    "public class Widget "
    "  public Tag: string; "
    "  public constructor() Tag := \"w\"; end constructor; "
    "end Widget; "
    "choice Hold Some(w: Widget), None; end Hold; "
    "public class Program "
    "  public static method Main(): int "
    "    var w := new Widget(); "
    "    var h := Hold.Some(w); "
    "    using var c := h; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (9) CONTROL — the `await using var` spelling of the SAME copy. §28.12 owns
// that spelling (#7306), so this fix must leave it EXACTLY as it is: it must
// not take the copy path, and it must not change the program's behaviour.
//
// The row asserts only what is TRUE ON BOTH ARMS: the program compiles, runs,
// exits 0 and prints "done". It deliberately asserts NO leak verdict for this
// spelling: on the base arm the `await using var` copy carries a PRE-EXISTING
// 35-byte leak (measured; see the commit body and the hand-back), which
// belongs to #7306 and is NOT pinned here — pinning it would freeze a defect
// in this file, and asserting its absence would fail on both arms.
//
// ⚠ RE-SPELLED (fix #7326 / #7396, §28.12 v0.345). The program used to place
// the `await using var` binding in a PLAIN `Main` — and §28.12 says
// "`await using` is valid only inside an async method or async lambda", so
// that program is an INVALID program and is now rejected at the declaration
// (the compiler enforced §30.2's twin for the `await` EXPRESSION but never
// §28.12's sentence for the `await using` BINDING). The control's INTENT —
// "the `await using var` spelling is unaffected by the #7316 copy fix" — is
// preserved by moving the binding into the ONE context §28.12 admits, an
// `async Main`: the spelling, the copy, and the printed output are the same,
// and the row still says "the program compiles, runs, exits 0 and prints
// 'done'". What changed is only the enclosing method's async-ness.
const char* kAwaitUsingVarChoiceCopy =
    "module M7316i; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static async method Main(): Task "
    "    var a := Msg.Note(\"a\" + \"b\"); "
    "    await using var b := a; "
    "    Console.WriteLine(\"done\"); "
    "  end Main; "
    "end Program; ";

// (10) §28.10's non-reassignability role. The `using var` spelling of a COPY
// must STILL be a scope pin: reassigning it is a compile error. A fix that
// dropped the pin would be as wrong as one that rejected the spelling.
const char* kUsingVarChoiceCopyReassigned =
    "module M7316j; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    var a := Msg.Note(\"a\" + \"b\"); "
    "    using var b := a; "
    "    b := Msg.Note(\"c\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// ── the leak oracle ────────────────────────────────────────────────────────
// Assert: the program compiled, linked, ran to completion UNDER ASan/LSan
// with NO leak, exited with `expectedExit`, and printed exactly `expectedOut`.
//
// `runAotChecked` is the CHECKED oracle: it sets `detect_leaks=1`, so a leak
// is a sanitizer failure and `r.ran` stays false. `runAot` would be VACUOUS
// here — it pins `detect_leaks=0`.
//
// The toolchain skip is asserted, not absorbed: a `return` on a SKIP would
// make an untested memory claim indistinguishable from a pass.
void expectNoLeak(const char* source, int expectedExit,
                  const char* expectedOut = nullptr) {
    JitResult r = runAotChecked(source);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "DIAG [%d]: %s\n", (int)d.severity,
                         d.message.c_str());
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, expectedExit);
    if (expectedOut) {
        std::string out;
        for (char c : r.stdoutText) {
            if (c == '\n') continue;
            out.push_back(c);
        }
        BEZEL_EXPECT_TRUE(out == expectedOut);
        if (out != expectedOut)
            std::fprintf(stderr, "STDOUT: got \"%s\", want \"%s\"\n",
                         out.c_str(), expectedOut);
    }
}

// ── the diagnostic oracle (the ownership pass, in-process) ─────────────────
// `runAot` DISCARDS a successful compile's stderr, so the §28.10 warning is
// invisible to it. This drives the OWNERSHIP PASS (the pass that emits the
// lint) over the test's OWN DiagnosticEngine, which records EVERY severity.
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

int countWarningSubstring(const OwnershipResult& r, const char* needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) ++n;
    }
    return n;
}

void dump(const OwnershipResult& r) {
    std::fprintf(stderr, "  hasErrors=%d diagnostics (%zu):\n",
                 (int)r.diagnostics.hasErrors(),
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%d] %s\n", (int)d.severity,
                     d.message.c_str());
}

}  // namespace

// ===========================================================================
// ROW 1 — LEAK: the `using var` spelling of a COPY of a value-like choice
// must release the copy's own reference to the payload string at scope end,
// exactly as the plain-`var` twin does. RED before the fix: the base arm
// leaked 3535 byte(s) in 202 allocations, so `runAotChecked` reported a
// sanitizer failure and `r.ran` was false ("assertion failed: r.ran").
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           using_var_choice_copy_releases_the_payload_at_scope_end) {
    expectNoLeak(kUsingVarChoiceCopyLoop, 0, "101");
}

// ===========================================================================
// ROW 2 — LEAK + OVER-RELEASE PROBE: an alias of the `using var` copy. Each
// armed copy takes its own reference at bind and releases it at scope end,
// while the source owns the construction's +1 and releases it at scope end. A
// fix that released the payload twice would drive the refcount negative —
// "refcount over-release (string)", an unconditional abort in the runtime —
// and the row must see that as a FAILURE, not a pass.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           using_var_choice_copy_with_an_alias_is_not_double_released) {
    expectNoLeak(kUsingVarChoiceCopyAliasedLoop, 0, "101");
}

// ===========================================================================
// ROW 3 — LEAK: a CHAIN of two `using var` copies. Every link takes its own
// reference at bind and drops it at scope end, and the source still drops the
// construction's +1. This is the row that catches a fix that armed only the
// first copy.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           using_var_choice_copy_chain_releases_every_link) {
    expectNoLeak(kUsingVarChoiceCopyChainLoop, 0, "101");
}

// ===========================================================================
// ROW 4 — CONTROL: the ONE-SHOT copy shape, the issue's literal repro. NOT a
// leak oracle (LSan reads the dead frame's stale pointer as a root — see the
// header note); the row pins that the shape COMPILES, RUNS, exits 0 and
// prints "done".
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           using_var_choice_copy_once_compiles_and_runs) {
    JitResult r = runAot(kUsingVarChoiceCopyOnce);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out;
    for (char c : r.stdoutText) {
        if (c == '\n') continue;
        out.push_back(c);
    }
    BEZEL_EXPECT_TRUE(out == "done");
}

// ===========================================================================
// ROW 5 — CONTROL: the plain-`var` copy twin of row 1. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           var_choice_copy_is_still_clean) {
    expectNoLeak(kVarChoiceCopy, 0, "done");
}

// ===========================================================================
// ROW 6 — CONTROL: #7305's shape — the `using var` CONSTRUCTION of the same
// value-like choice. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           using_var_choice_construction_is_still_clean) {
    expectNoLeak(kUsingVarChoiceConstruction, 0, "done");
}

// ===========================================================================
// ROW 7 — CONTROL: a COPY of a choice whose payload is an INT. The scope-end
// walk touches only string-typed payload slots, so the fix must not have
// widened it. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           using_var_choice_copy_of_an_int_is_still_clean) {
    expectNoLeak(kUsingVarIntPayloadCopy, 0, "done");
}

// ===========================================================================
// ROW 8 — CONTROL: a COPY of a CLASS-typed (borrow-bearing) choice. §35.4
// classifies it BorrowBearing, so the value-like payload path does not apply
// to it and the fix must not have changed it. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           using_var_choice_copy_of_a_class_choice_is_still_clean) {
    expectNoLeak(kUsingVarClassChoiceCopy, 0, "done");
}

// ===========================================================================
// ROW 9 — CONTROL: the `await using var` spelling of the same copy must be
// UNCHANGED by this fix (§28.12 owns that spelling; #7306 is its issue).
// The row asserts the program's behaviour — it compiled, ran, exited 0 and
// printed "done" — and deliberately asserts NO leak verdict for it (see the
// program's comment: that spelling carries a PRE-EXISTING leak that belongs
// to #7306). Green on BOTH arms.
//
// ⚠ The program was RE-SPELLED into an `async Main` by fix #7326 / #7396
// (§28.12, v0.345) — the `await using var` binding is now enforced to sit in
// an async context, so the control had to move there to stay a legal
// program. The row's EXPECTATIONS are unchanged: the program compiles, runs,
// exits 0 and prints "done".
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           await_using_var_choice_copy_is_unchanged) {
    JitResult r = runAot(kAwaitUsingVarChoiceCopy);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out;
    for (char c : r.stdoutText) {
        if (c == '\n') continue;
        out.push_back(c);
    }
    BEZEL_EXPECT_TRUE(out == "done");
}

// ===========================================================================
// ROW 10 — DIAGNOSTIC: the §28.10 "vacuous" warning STILL fires for the
// `using var` spelling of the copy. The lint is CORRECT about the OBLIGATION
// (a value-typed value has no disposal obligation, §28.9) and is kept by the
// fix — the scope-end RELEASE of the runtime's own reference is not a
// disposal obligation. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           using_var_choice_copy_still_warns_vacuous) {
    OwnershipResult r = parseAndCheckOwnership(kUsingVarChoiceCopyOnce);
    dump(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuous), 1);
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuousObligation), 1);
}

// ===========================================================================
// ROW 11 — §28.10's non-reassignability role: the `using var` spelling of a
// COPY must STILL be a scope pin. GREEN ON BOTH ARMS.
//
// The rejection must be a REAL compile-time diagnostic with a real source
// location — not a crash, a timeout or a sanitizer report.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_copy_7316,
           using_var_choice_copy_is_still_not_reassignable) {
    JitResult r = runAot(kUsingVarChoiceCopyReassigned);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kReassign));
}
