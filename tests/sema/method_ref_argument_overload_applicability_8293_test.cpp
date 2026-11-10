// Spec-derived regression test for issue #8293.
//
// SPEC §22.6 (v0.355), *Overloading* — applicability is decided PER CANDIDATE,
// and a method-reference ARGUMENT has no static type of its own until it is
// formed against a target:
//
//   "A method-reference argument ([§27.6](#sec-method-references)) likewise has
//    no static type of its own until it is formed against a target; at an
//    overloaded callee it is formed per candidate against that candidate's
//    parameter type ... and it ranks as a lambda literal does."
//
// SPEC §27.6 (v0.355), *Method references to overloaded methods*:
//
//   "When the named method (or property) is overloaded, the reference is
//    resolved using the **target function type** at the use site."
//
// and the no-target rule (the same paragraph):
//
//   "When no target type is available (e.g., `var f := Math.Abs;` with no
//    annotation), the reference is ambiguous and the compiler reports an error
//    pointing to the candidate overloads."
//
// THE DEFECT (measured on this fix's base, `17b59c39`): an overloaded method
// reference passed as an ARGUMENT to an overloaded callee whose candidates are
// {`(Object)`, `((int) -> int)`} was bound by the CANDIDATES' DECLARATION
// ORDER, not by applicability. The `Object` slot supplies no function type, so
// the overloaded reference cannot be formed there at all (§27.6's no-target
// rule) — that candidate is INAPPLICABLE, never an error reported at that
// candidate — while at the `((int) -> int)` slot the reference forms against
// the target and selects `Len(x: int)`. The function-typed candidate is
// therefore the only applicable one in BOTH declaration orders.
//
// MEASURED on the base (`17b59c39`), with the `Show(o: Object)` overload
// declared FIRST:
//
//   U.Show(V.Len);   REFUSED — "the method reference `Len` is overloaded and
//                    has no target type to select by (§27.6 v0.307)"
//
// and with the `Show(f: (int) -> int)` overload declared FIRST:
//
//   U.Show(V.Len);   COMPILES, prints `105`
//
// ⇒ the two declaration orders disagreed. On the tip both print `105`.
//
// ⇒⇒⇒ NOTE ON THE ISSUE'S OWN BASE BEHAVIOUR: the issue reports the
// `Object`-first order as printing `obj`; that was measured on the older
// staging tip `6efe5fbf`. On THIS lane's base (`17b59c39`, which already
// carries #8294/#8295 and the #8205 narrowing) the same program is REFUSED
// instead — the over-admission was turned into an over-refusal by #8205, but
// the ORDER DEPENDENCE the issue is about is unchanged and is what this file
// pins.
//
// THE CONTROLS below are the shapes that must NOT move: the §27.6 no-target
// rule (a reference with no target is still ambiguous, and still reported
// ONCE), the target-typed formation (both arities), the single-candidate
// callee, a callee with no function slot at all, the non-overloaded reference
// (which CAN be formed at an `Object` slot, so the `Object` candidate is the
// applicable one), the already-formed function value, the lambda argument, and
// the EXTENSION-member argument — the last one is the documented SCOPE
// BOUNDARY of this fix (the extension path's §26.5 rule-4 tie is reported by
// the lookup at the arity the target selects, and a report must not be made
// for a candidate the call may not select).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

std::size_t errorCount(const JitResult& r, const std::string& needle) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        for (std::size_t at = d.message.find(needle); at != std::string::npos;
             at = d.message.find(needle, at + needle.size()))
            ++n;
    }
    return n;
}

// §27.6's no-target rule — the diagnostic a reference with NO target type
// gets.
const char kNoTarget[] =
    "the method reference `Len` is overloaded and has no target type to "
    "select by";

// The overloaded group the reference names: `Len()` and `Len(x: int)`.
const char kGroup[] =
    "class V\n"
    "  public static method Len(): int => 42;\n"
    "  public static method Len(x: int): int => x + 100;\n"
    "  public static method One(): int => 7;\n"
    "end V;\n";

// The two DECLARATION ORDERS of the callee's overloads — the whole defect is
// that they disagreed.
const char kCalleeObjectFirst[] =
    "class U\n"
    "  public static method Show(o: Object): () => Console.WriteLine(\"obj\");\n"
    "  public static method Show(f: (int) -> int): () => "
    "Console.WriteLine(f(5));\n"
    "end U;\n";

const char kCalleeFunctionFirst[] =
    "class U\n"
    "  public static method Show(f: (int) -> int): () => "
    "Console.WriteLine(f(5));\n"
    "  public static method Show(o: Object): () => Console.WriteLine(\"obj\");\n"
    "end U;\n";

// A program with `body` in `Main`, over the overload group and the callee
// declarations given.
std::string program(const std::string& callee, const std::string& body) {
    return std::string("module M;\n") + kGroup + callee +
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectCheckedStdout(const std::string& src, const std::string& out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// A program that forms a function value INLINE as a call argument leaks
// 64 B — MEASURED on this fix's BASE (`17b59c39`) for every such program
// (`U.Show(V.Len)`, `U.Show(lambda(x: int) => x + 100)`, …) and IDENTICALLY
// on the tip: the leak is a PRE-EXISTING, SEPARATE family (the function
// value's own lifetime at an inline-argument position), it is NOT this
// issue's, and this fix neither causes it nor worsens it. Under
// `runAotChecked` the LeakSanitizer SUMMARY is a sanitizer REPORT, and the
// harness fails the row on it — so the row would be measuring the leak
// instead of the SELECTION. `runAotCheckedLenient` keeps the
// generated-code AddressSanitizer armed (a use-after-free / out-of-bounds
// read in an emitted load still fails the row) and turns only the leak
// detector off.
void expectLenientStdout(const std::string& src, const std::string& out) {
    JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

void expectRejected(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

}  // namespace

// ── THE REPRO — the two declaration orders must AGREE ──────────────────────
//
// per §22.6 (v0.355), *Method-reference arguments*: the reference is formed
// PER CANDIDATE against that candidate's parameter type; per §27.6 (v0.355),
// *Method references to overloaded methods*: an overloaded reference with no
// function type to resolve by is ambiguous — so the `Object` candidate is
// INAPPLICABLE and the function-typed candidate is the only applicable one.

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           the_object_candidate_declared_first_selects_the_function_candidate) {
    // per §22.6 / §27.6 (v0.355): the issue's literal repro, with the
    // `Object` overload declared first
    expectLenientStdout(program(kCalleeObjectFirst, "U.Show(V.Len);"),
                        "105\n");
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           the_function_candidate_declared_first_selects_the_function_candidate) {
    // per §22.6 / §27.6 (v0.355): the same program with the two `Show`
    // overloads' declaration order SWAPPED — ⇒ THE TWO ORDERS MUST AGREE
    expectLenientStdout(program(kCalleeFunctionFirst, "U.Show(V.Len);"),
                        "105\n");
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           an_instance_method_reference_selects_the_function_candidate) {
    // per §22.6 / §27.6 (v0.355): the same shape over an INSTANCE member —
    // the reference `v.Len` denotes the group's `Len(x: int)` at the
    // function-typed slot, and the `Object` slot remains inapplicable
    expectLenientStdout(
        std::string("module M;\n"
                    "class V\n"
                    "  public constructor() end constructor;\n"
                    "  public method Len(): int => 42;\n"
                    "  public method Len(x: int): int => x + 100;\n"
                    "end V;\n") +
            kCalleeObjectFirst +
            "class C\n"
            "  public static method Main(): int\n"
            "    var v := new V();\n"
            "    U.Show(v.Len);\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n",
        "105\n");
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           two_reference_arguments_select_the_function_candidate) {
    // per §22.6 / §27.6 (v0.355): the formation is PER ARGUMENT POSITION —
    // two overloaded references at two positions both form at the
    // function-typed slot
    expectLenientStdout(program(kCalleeObjectFirst,
                                "U.Show(V.Len);\n"
                                "    U.Show(V.Len);"),
                        "105\n105\n");
}

// ── THE CONTROLS — every one of these must be UNCHANGED ────────────────────

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           a_reference_with_no_target_is_still_refused) {
    // control: per §27.6 (v0.355) — "the reference is ambiguous and the
    // compiler reports an error pointing to the candidate overloads"
    expectRejected(program(kCalleeObjectFirst, "var f := V.Len;"), kNoTarget);
}

    // AMBIGUOUS-RAN-FALSE-DEBT: #8293 the row's message check goes through an in-file helper over the diagnostics list, which the ratchet does not recognise as protection; the row is tracked as debt until the protection is spelled in the recognised form
BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293, a_reference_with_no_target_is_reported_once) {
    // control: §8205's one-report-per-reference discipline
    auto r = runAot(program(kCalleeObjectFirst, "var f := V.Len;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(errorCount(r, kNoTarget), static_cast<std::size_t>(1));
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           the_target_typed_formation_still_selects_the_arity_one_overload) {
    // control: per §27.6 (v0.355) — the target `(int) -> int` selects
    // `Len(x: int)`, so the call is `f(5)` = 105
    expectCheckedStdout(program(kCalleeObjectFirst,
                                "var f: (int) -> int := V.Len;\n"
                                "    Console.WriteLine(f(5));"),
                        "105\n");
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           the_target_typed_formation_still_selects_the_arity_zero_overload) {
    // control: per §27.6 (v0.355) — the target `() -> int` selects the
    // arity-0 `Len()`, so the call is `f()` = 42
    expectCheckedStdout(program(kCalleeObjectFirst,
                                "var f: () -> int := V.Len;\n"
                                "    Console.WriteLine(f());"),
                        "42\n");
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           the_single_candidate_callee_is_unchanged) {
    // control: a callee with ONE `Show(f: (int) -> int)` — the single-candidate
    // case must not regress
    expectLenientStdout(program("class U\n"
                                "  public static method Show(f: (int) -> int): ()"
                                " => Console.WriteLine(f(5));\n"
                                "end U;\n",
                                "U.Show(V.Len);"),
                        "105\n");
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           a_callee_with_no_function_slot_is_still_refused) {
    // control: per §22.6 (v0.355) — the candidates are `{Object, string}` and
    // NEITHER is a function slot, so the overloaded reference can be formed at
    // neither ⇒ the call has no applicable candidate ⇒ it is refused. ⇒ THE
    // FIX MUST NOT WIDEN THE SELECTION.
    expectRejected(program("class U\n"
                           "  public static method Show(o: Object): () => "
                           "Console.WriteLine(\"obj\");\n"
                           "  public static method Show(s: string): () => "
                           "Console.WriteLine(\"str\");\n"
                           "end U;\n",
                           "U.Show(V.Len);"),
                   kNoTarget);
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           a_callee_with_only_an_object_candidate_is_still_refused) {
    // control: the `Object`-only callee — the reference cannot be formed at an
    // `Object` slot (§27.6's no-target rule), so the call is refused
    expectRejected(program("class U\n"
                           "  public static method Show(o: Object): () => "
                           "Console.WriteLine(\"obj\");\n"
                           "end U;\n",
                           "U.Show(V.Len);"),
                   kNoTarget);
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           a_non_overloaded_reference_is_unchanged) {
    // control: per §27.6 (v0.355) — `V.One` is NOT overloaded, so it CAN be
    // formed at the `Object` slot; that candidate is the applicable one and
    // the call prints `obj`
    expectLenientStdout(program(kCalleeObjectFirst, "U.Show(V.One);"),
                        "obj\n");
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           an_already_formed_function_value_is_unchanged) {
    // control: a FUNCTION VALUE (not a fresh reference) at the same callee —
    // §27.1.1's rebinding rule, not the §22.6 per-candidate formation
    expectCheckedStdout(program(kCalleeObjectFirst,
                                "var g: (int) -> int := lambda(x: int) => "
                                "x + 100;\n"
                                "    U.Show(g);"),
                        "105\n");
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           a_lambda_argument_is_unchanged) {
    // control: a lambda literal at the same callee — §27.4: a lambda is
    // target-typed by construction
    expectLenientStdout(program(kCalleeObjectFirst,
                                "U.Show(lambda(x: int) => x + 100);"),
                        "105\n");
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           a_preformed_reference_value_is_unchanged) {
    // control: the reference formed at a target and passed as a VALUE
    expectCheckedStdout(program(kCalleeObjectFirst,
                                "var f: (int) -> int := V.Len;\n"
                                "    U.Show(f);"),
                        "105\n");
}

BEZEL_TEST(sema_method_ref_argument_overload_applicability_8293,
           an_extension_member_reference_is_unchanged) {
    // control / SCOPE BOUNDARY: the EXTENSION-member path is deliberately NOT
    // covered by this fix. An extension member's §26.5 rule-4 tie is
    // reported by the lookup itself, at the arity the target selects — a
    // report that must not be made for a candidate the call may not select —
    // so the per-candidate formation does not run there. The extension
    // reference below is therefore still bound by declaration order: the
    // `Object` candidate is selected, and the reference (a bound method
    // reference over a local) carries a borrow, which §28.3's closure bullet
    // refuses at a set-erasing position.
    expectRejected(
        std::string("module M;\n"
                    "class V\n"
                    "  public constructor() end constructor;\n"
                    "end V;\n"
                    "extension V\n"
                    "  public method Ext(): int => 7;\n"
                    "end;\n") +
            kCalleeObjectFirst +
            "class C\n"
            "  public static method Main(): int\n"
            "    var v := new V();\n"
            "    U.Show(v.Ext);\n"
            "    return 0;\n"
            "  end Main;\n"
            "end C;\n",
        "function value carries captured borrow(s)");
}
