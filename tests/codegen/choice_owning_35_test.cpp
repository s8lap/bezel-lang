// Codegen tests for §35.4-§35.5 (v0.200, #2045): OWNING choices — the
// active-case disposal lowering. An owning-classified choice (§35.4) joins
// §15.3's move-only category: at scope end the active case's owned payloads
// are disposed in payload declaration order (the K3.3 / §35.4 rule "Disposal
// is defined by the active case"). These tests pin the active-case disposal
// code path that the codegen emission must generate for `using var` /
// scope-end disposal of an owning-choice value:
//
//   * §35.13 layout + §35.3 construction + active-case disposal: a choice
//     value with one owning-classified case carrying one owned class
//     payload disposes the payload exactly once at scope end (no leak, no
//     double-free) — verifiable via a destructor-instrumented class whose
//     destructor increments a static counter.
//
//   * Active-case disposal depends on the runtime TAG: a payload-free
//     case as the active case disposes nothing, an owned-payload case
//     disposes its owned field.
//
// Spec-cited regression test for issue #2045 stage 3 (owning-match +
// active-disposal).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Wrap a method body in the standard module+class shell and run it; return
// the exit code (-999 if the run failed). Mirrors choice_codegen_35_test.cpp.
//
// The wrapper names the host class `Host` (not `C`) so the test's own class
// declarations (`class C ... end C;`) — destructors-instrumented for the
// spec-cited disposal counter — do not collide with the wrapper's host class.
// Choice / class declarations are passed in `decls` and live at module scope.
int run(const std::string& decls, const std::string& body) {
    auto r = runAot(
        std::string("module M; ") + decls +
        std::string(" class Host public static method Main(): int ") +
        body + " end Main; end Host;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// --- §35.4 active-case disposal at scope end (using var) --------------------

BEZEL_TEST(codegen_choice_owning_35,
           spec_35_4_active_case_disposal_using_var_owned_payload) {
    // §35.4: an owning-classified choice's owned payloads are disposed at
    // scope end in payload declaration order. `Outcome.Fetched(new C())`
    // constructs the choice with the Fetched case active (the case payload
    // field `owned value: C` consumes the fresh `new C()`); `using var` at
    // scope end runs the active-case disposal path, which dispatches the
    // Fetched case's `value` field's destructor. C's destructor increments
    // _n once. Exit code = 1. (NB: a call-site `owned` marker on a fresh
    // producer is rejected by §22.3 — no live binding is invalidated, so
    // there is nothing to announce — the case payload's `owned` marker is
    // the load-bearing piece here.)
    auto r = run(
        "class C "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end C; "
        "choice Outcome Fetched(owned value: C), Missing; end Outcome;",
        "do using var o: Outcome := Outcome.Fetched(new C()); end; "
        "return C._n;");
    BEZEL_EXPECT_EQ(r, 1);
}

BEZEL_TEST(codegen_choice_owning_35,
           spec_35_4_active_case_disposal_payload_free_case_disposes_nothing) {
    // §35.4: a payload-free active case disposes nothing. `Outcome.Missing`
    // has no owned payload; the `using var` scope-end path takes the
    // Missing arm of the active-case disposal switch and dispatches no
    // destructor. _n stays 0.
    auto r = run(
        "class C "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end C; "
        "choice Outcome Fetched(owned value: C), Missing; end Outcome;",
        "do using var o: Outcome := Outcome.Missing; end; "
        "return C._n;");
    BEZEL_EXPECT_EQ(r, 0);
}

BEZEL_TEST(codegen_choice_owning_35,
           spec_35_4_active_case_disposal_two_cases_picks_correct_one) {
    // §35.4: the active-case disposal switch dispatches on the TAG. If the
    // active case is Fetched (tag=0), Fetched.value is disposed. If
    // Missing (tag=1), nothing is disposed. We exercise both via separate
    // methods, then return the sum of disposals: 1 (Fetched) + 0 (Missing)
    // = 1.
    auto r = run(
        "class C "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end C; "
        "choice Outcome Fetched(owned value: C), Missing; end Outcome; "
        "class Helper "
        "  public static method DoFetched(): int "
        "    do using var o: Outcome := Outcome.Fetched(new C()); end; "
        "    return 0; "
        "  end DoFetched; "
        "  public static method DoMissing(): int "
        "    do using var o: Outcome := Outcome.Missing; end; "
        "    return 0; "
        "  end DoMissing; "
        "end Helper;",
        "var a: int := Helper.DoFetched(); "
        "var b: int := Helper.DoMissing(); "
        "return C._n;");
    BEZEL_EXPECT_EQ(r, 1);
}

BEZEL_TEST(codegen_choice_owning_35,
           spec_35_4_active_case_disposal_multi_field_owned_payloads) {
    // §35.4: payloads are disposed "in payload declaration order." A case
    // with two owned class fields disposes both — destruction counter goes
    // up by 2. Both classes share _n so we can observe the sum.
    auto r = run(
        "class C "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end C; "
        "choice Outcome Fetched(owned first: C, owned second: C), "
        "        Missing; end Outcome;",
        "do using var o: Outcome := "
        "     Outcome.Fetched(new C(), new C()); end; "
        "return C._n;");
    BEZEL_EXPECT_EQ(r, 2);
}
