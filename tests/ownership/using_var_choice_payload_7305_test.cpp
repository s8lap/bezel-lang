// Spec-derived regression test for issue #7305 — a `using var` local of a
// VALUE-LIKE CHOICE PAYLOAD leaked the payload, while the plain-`var` twin of
// the SAME program was clean.
//
// THE SPEC ANCHOR — §28.10 "`using var`" (v0.344), the runtime-managed
// paragraph, verbatim:
//
//   "`using var` accepts a value-type or runtime-managed right-hand side. For
//    ordinary value types and runtime-managed types, the disposal at scope end
//    is a no-op (these types do not require disposal per §28.9), and the only
//    effect is the reassignability restriction."
//
// ⇒ THREE requirements, all pinned here:
//
//   (1) "the disposal at scope end is a no-op" is about the DISPOSAL
//       OBLIGATION: a value-like choice is an ordinary copyable VALUE type
//       (§35.4), it has no `Dispose()` to call. It is NOT a licence to skip
//       releasing the runtime's own reference — the +1 the §35.3 construction
//       minted on the payload string, which the plain-`var` twin owns and
//       releases at scope end. (Rows 1-3.)
//   (2) "and the only effect is the reassignability restriction" — the
//       `using var` spelling of a runtime-managed value must have the SAME
//       runtime lifetime as the plain-`var` spelling. One released the payload
//       and the other did not: a direct contradiction of that clause.
//       (Rows 1-3, and the `var`/`const` control rows 4-5.)
//   (3) The reassignability restriction itself must SURVIVE the fix — a
//       `using var` is a scope pin (axiom A5), and the §28.10 lint that
//       reports the vacuous spelling must still fire. (Rows 10-11.)
//
// §28.9 "Runtime-managed types" (v0.344) supplies the carve the fix rests on:
// the runtime manages the payload string's lifecycle, and §28.9's rule for a
// runtime-managed value is that the COMPILER does not warn about undisposed
// runtime-managed values — there is no disposal OBLIGATION on this value, so
// the only thing that can be wrong here is the RUNTIME LIFETIME. §28.3's K1
// (the ownership kernel) makes the same point for the binding: a binding has
// EXACTLY ONE ownership classification (owned / borrowed / unclassified), and
// the `using var` spelling of a value the plain-`var` spelling classifies as
// OWNED may not be left UNCLASSIFIED. §35.4 supplies the classification the
// fix keys on: a value-like choice is "an ordinary copyable value type", and
// its payload's ownership is "defined by the active case".
//
// THE DEFECT, MEASURED (the base arm, `bezelc --sanitize=address`, LSan):
//
//   using var c := Msg.Note("a" + "b");  ->  35 byte(s) leaked in 2 allocations
//   var         c := Msg.Note("a" + "b");  ->  clean
//
// ⇒ THE TWO-SPELLING MATRIX, with the `using var` arm the odd one out.
//
// THE SITE. `Emitter::collectStringReleaseEligible` (src/codegen/
// LlvmBackend_Setup.cpp) — its candidate walk admitted the `using var`
// SPELLING of a +1 STRING producer (fix #7272), but a choice-payload
// constructor is not a string producer, so a `using var` value-like choice
// local fell out of the walk entirely: it was never a candidate, never entered
// `valueLikeChoiceStrReleaseEligible_`, and `armValueLikeChoiceStrRelease`
// never armed its scope-end release — the §35.3 construction's +1 on the
// payload string was never discharged.
//
// THE `using var` DECLARATION'S OWN DISPOSAL PATH CANNOT COVER IT: a
// value-like choice is a value type, not a class, so `registerUsingDisposal`
// classifies nothing for it — and §28.10 says of it that "the disposal at
// scope end is a no-op". The compiler's own §28.10 lint agrees ("`using var`
// `c` is vacuous — the RHS is a value-typed/runtime-managed value with no
// disposal obligation (§28.9)"), and that lint is CORRECT about the
// OBLIGATION and is kept (row 10).
//
// ── THE ORACLES (and why they differ per row) ─────────────────────────────
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
//   1. LEAK          `using var` + a value-like choice payload (a +1 string
//                    producer argument) -> no leak (RED before)
//   2. LEAK          the same, 101 iterations in a loop -> no leak (RED before)
//   3. LEAK + UAF    the DOUBLE-RELEASE probe: two aliases of the same
//                    `using var` value — no leak AND no over-release
//   4. CONTROL       the plain-`var` twin of row 1             (green both)
//   5. CONTROL       the §21.1 `const` twin of row 1           (green both)
//   6. CONTROL       `using var` of a LITERAL payload (an immortal sentinel)
//   7. CONTROL       `using var` of a PAYLOAD-FREE case
//   8. CONTROL       `using var` of a choice with an INT payload
//   9. CONTROL       `using var` of a CLASS-typed (borrow-bearing) choice —
//                    the fix is scoped to the VALUE-LIKE path
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

// (1) THE DEFECT. `using var` + a value-like choice payload.
const char* kUsingVarChoice =
    "module M7305a; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    using var c := Msg.Note(\"a\" + \"b\"); "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (2) THE REPEATED SHAPE. A single 35-byte leak is easy to miss; 101 of them
// (3535 bytes in 202 allocations — `0..100` is INCLUSIVE, so the body runs
// 101 times) is not.
const char* kUsingVarChoiceLoop =
    "module M7305b; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    var n := 0; "
    "    for var i in 0..100 do "
    "      using var c := Msg.Note(\"a\" + \"b\"); "
    "      n := n + 1; "
    "    end for; "
    "    Console.WriteLine(n.ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (3) THE DOUBLE-RELEASE PROBE. Two aliases of the SAME `using var` value.
// Each alias is an ARMED copy, so it takes its own reference at bind and
// releases it at scope end, while the `using var` local owns the
// construction's +1 and releases it at scope end. A fix that released the
// payload TWICE would drive the refcount negative — "refcount over-release
// (string)", an unconditional abort in
// src/runtime/bezel_runtime_refcounting.cpp — which the ASan arm of this row
// catches as a FAILURE, not a pass. The row also pins that the aliases' own
// references are balanced (an alias WITHOUT its retain would be a
// use-after-free, also caught by ASan).
//
// NB — THIS ROW IS GREEN ON BOTH ARMS, AND THAT IS THE POINT: it is the
// DOUBLE-RELEASE GUARD, not a leak row. On the base arm this shape DOES leak
// 35 byte(s) in 2 allocations — but LSan does not report it, because the
// harness runs LeakSanitizer with its DEFAULT stack scan and a stale pointer
// to the payload survives in the dead `Main` frame, which LSan reads as a
// root. Re-measured with `LSAN_OPTIONS=use_stacks=0` the base arm reports
// "SUMMARY: AddressSanitizer: 35 byte(s) leaked in 2 allocation(s)" and the
// fix arm is clean. Rows 1 and 2 are NOT masked that way (they FAIL on the
// base arm), so the leak is pinned there; this row is here for the
// over-release half, which no LSan setting can mask.
const char* kUsingVarChoiceTwoAliases =
    "module M7305c; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    using var c := Msg.Note(\"a\" + \"b\"); "
    "    var d := c; "
    "    var e := c; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (4) CONTROL — the plain-`var` twin of row 1. Green on BOTH arms: it is the
// reference behaviour the fix must reproduce, and it proves the fix did not
// change the reference path.
const char* kVarChoice =
    "module M7305d; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    var c := Msg.Note(\"a\" + \"b\"); "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (5) CONTROL — the §21.1 runtime `const` twin of row 1. A `const` is never a
// `using var` (by grammar), and it was classified exactly as its `var` twin
// since fix #3416. Green on BOTH arms.
const char* kConstChoice =
    "module M7305e; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    const c := Msg.Note(\"a\" + \"b\"); "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (6) CONTROL — a `using var` of a LITERAL payload (an immortal sentinel, on
// which retain and release are no-ops). Green on BOTH arms.
const char* kUsingVarLiteralPayload =
    "module M7305f; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    using var c := Msg.Note(\"lit\"); "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (7) CONTROL — a `using var` of a PAYLOAD-FREE case. The slot holds no
// string, so there is nothing to release. Green on BOTH arms.
const char* kUsingVarPayloadFree =
    "module M7305g; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    using var c := Msg.Empty; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (8) CONTROL — a `using var` of a choice whose payload is an INT (a value
// type with no runtime-managed slot). The scope-end walk is a no-op here, so
// the row pins that the fix did not widen the walk to non-string payloads.
// Green on BOTH arms.
const char* kUsingVarIntPayload =
    "module M7305h; "
    "choice Num N(v: int), Z; end Num; "
    "public class Program "
    "  public static method Main(): int "
    "    using var c := Num.N(5); "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (9) CONTROL — a `using var` of a CLASS-typed (borrow-bearing) choice. §35.4
// classifies it BorrowBearing, not ValueLike, so the value-like payload path
// does not apply to it at all: the fix must NOT change it. Green on BOTH arms.
const char* kUsingVarClassChoice =
    "module M7305i; "
    "public class Widget "
    "  public Tag: string; "
    "  public constructor() Tag := \"w\"; end constructor; "
    "end Widget; "
    "choice Hold Some(w: Widget), None; end Hold; "
    "public class Program "
    "  public static method Main(): int "
    "    var w := new Widget(); "
    "    using var c := Hold.Some(w); "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (10) §28.10's non-reassignability role. The `using var` spelling of a
// value-like choice must STILL be a scope pin: reassigning it is a compile
// error. A fix that dropped the pin would be as wrong as one that rejected
// the spelling.
const char* kUsingVarChoiceReassigned =
    "module M7305j; "
    "choice Msg Note(text: string), Empty; end Msg; "
    "public class Program "
    "  public static method Main(): int "
    "    using var c := Msg.Note(\"a\" + \"b\"); "
    "    c := Msg.Note(\"c\"); "
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
// ROW 1 — LEAK: the `using var` spelling of a value-like choice payload must
// release the construction's +1 on the payload string at scope end. RED before
// the fix: the base arm leaked 35 byte(s) in 2 allocations, so
// `runAotChecked` reported a sanitizer failure and `r.ran` was false.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           using_var_choice_payload_releases_the_payload_at_scope_end) {
    expectNoLeak(kUsingVarChoice, 0, "done");
}

// ===========================================================================
// ROW 2 — LEAK: the REPEATED shape (101 iterations — `0..100` is inclusive).
// A single 35-byte leak is easy to miss; 101 of them are not. RED before the
// fix: 3535 byte(s) in 202 allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           using_var_choice_payload_releases_it_in_a_loop) {
    expectNoLeak(kUsingVarChoiceLoop, 0, "101");
}

// ===========================================================================
// ROW 3 — LEAK + DOUBLE-RELEASE PROBE: two aliases of the same `using var`
// value. Each alias is an armed copy: it takes its own reference at bind and
// releases it at scope end, while the `using var` local owns the
// construction's +1 and releases it at scope end. A fix that released the
// payload twice would drive the refcount negative — "refcount over-release
// (string)", an unconditional abort in the runtime — and the row must see
// that as a FAILURE, not a pass. RED before the fix: 35 byte(s) in 2
// allocations.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           using_var_choice_payload_with_two_aliases_is_not_double_released) {
    expectNoLeak(kUsingVarChoiceTwoAliases, 0, "done");
}

// ===========================================================================
// ROW 4 — CONTROL: the plain-`var` twin of row 1. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           var_choice_payload_is_still_clean) {
    expectNoLeak(kVarChoice, 0, "done");
}

// ===========================================================================
// ROW 5 — CONTROL: the §21.1 runtime `const` twin of row 1. Green on BOTH
// arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           const_choice_payload_is_still_clean) {
    expectNoLeak(kConstChoice, 0, "done");
}

// ===========================================================================
// ROW 6 — CONTROL: a `using var` of a LITERAL payload (an immortal sentinel,
// on which retain and release are no-ops). Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           using_var_choice_payload_of_a_literal_is_still_clean) {
    expectNoLeak(kUsingVarLiteralPayload, 0, "done");
}

// ===========================================================================
// ROW 7 — CONTROL: a `using var` of a PAYLOAD-FREE case — nothing to release.
// Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           using_var_choice_payload_without_a_payload_is_still_clean) {
    expectNoLeak(kUsingVarPayloadFree, 0, "done");
}

// ===========================================================================
// ROW 8 — CONTROL: a `using var` of a choice whose payload is an INT. The
// scope-end walk touches only string-typed payload slots, so the fix must
// not have widened it. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           using_var_choice_payload_of_an_int_is_still_clean) {
    expectNoLeak(kUsingVarIntPayload, 0, "done");
}

// ===========================================================================
// ROW 9 — CONTROL: a `using var` of a CLASS-typed (borrow-bearing) choice.
// §35.4 classifies it BorrowBearing, so the value-like payload path does not
// apply to it and the fix must not have changed it. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           using_var_class_typed_choice_is_still_clean) {
    expectNoLeak(kUsingVarClassChoice, 0, "done");
}

// ===========================================================================
// ROW 10 — DIAGNOSTIC: the §28.10 "vacuous" warning STILL fires for the
// `using var` spelling. The lint is CORRECT about the OBLIGATION (a
// value-typed value has no disposal obligation, §28.9) and is kept by the
// fix — the scope-end RELEASE of the runtime's own reference is not a
// disposal obligation. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           using_var_choice_payload_still_warns_vacuous) {
    OwnershipResult r = parseAndCheckOwnership(kUsingVarChoice);
    dump(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuous), 1);
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuousObligation), 1);
}

// ===========================================================================
// ROW 11 — DIAGNOSTIC: the §28.10 "vacuous" warning does NOT fire for the
// plain-`var` twin. Green on BOTH arms.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           var_choice_payload_does_not_warn_vacuous) {
    OwnershipResult r = parseAndCheckOwnership(kVarChoice);
    dump(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(countWarningSubstring(r, kVacuous), 0);
}

// ===========================================================================
// ROW 12 — §28.10's non-reassignability role: the `using var` spelling of a
// value-like choice must STILL be a scope pin. GREEN ON BOTH ARMS.
//
// The rejection must be a REAL compile-time diagnostic with a real source
// location — not a crash, a timeout or a sanitizer report.
// ===========================================================================
BEZEL_TEST(ownership_using_var_choice_payload_7305,
           using_var_choice_payload_is_still_not_reassignable) {
    JitResult r = runAot(kUsingVarChoiceReassigned);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kReassign));
}
