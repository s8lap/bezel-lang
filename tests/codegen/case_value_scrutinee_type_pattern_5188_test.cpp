// Codegen/AOT pins for #5188 (§19.3 v0.311, restated v0.313; Appendix C.1;
// §35.5): a class, interface, `string` or `Object` type pattern over a
// value-like scrutinee (choice, unit, ...) is a compile-time error, reported
// at a real source location with the specific diagnostic; a choice case
// pattern named like an in-scope class (`Some(var x)` beside `class Some`)
// is still the case pattern (§35.5) and runs.
//
// NOT covered (residual of #5188): "a pattern naming a different value type"
// (`case 5 of Color`) is not yet rejected.

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
constexpr const char* kNeedle =
    "value-type scrutinee can never match a reference-typed";

const std::string kDecls =
    "module M; "
    "interface IShape method M(): int; end IShape; "
    "choice Outcome Fetched(body: int), NotModified; end Outcome; ";

void expectRejected(const std::string& body) {
    auto r = runAot(kDecls + "class C public static method Main(): int " + body +
                    " end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle));
}
}  // namespace

// Issue probe 1: bare interface pattern over a value-like choice.
BEZEL_TEST(case_value_scrutinee_type_pattern_5188, choice_bare_interface_rejected) {
    expectRejected("var c := Outcome.Fetched(7); "
                   "return (case c of IShape => 1, _ => 2);");
}

// Issue probe 2: binding interface pattern over a value-like choice.
BEZEL_TEST(case_value_scrutinee_type_pattern_5188, choice_binding_interface_rejected) {
    expectRejected("var c := Outcome.Fetched(7); "
                   "return (case c of var s: IShape => 1, _ => 2);");
}

// Issue probe 3: `Object` pattern over a unit scrutinee.
BEZEL_TEST(case_value_scrutinee_type_pattern_5188, unit_object_rejected) {
    expectRejected("return (case () of Object => 1, _ => 2);");
}

// §35.5 / C.1: a choice case named `Some` beside an in-scope class `Some` is
// still the case pattern, not a type test; runs and yields 7.
BEZEL_TEST(case_value_scrutinee_type_pattern_5188, case_pattern_named_like_class_runs) {
    auto r = runAot(
        "module M; "
        "choice Outcome Some(v: int), None; end Outcome; "
        "class Some public method M(): int return 0; end M; end Some; "
        "class C public static method Main(): int "
        "  var o := Outcome.Some(7); "
        "  var r := (case o of Some(var x) => x, _ => 0); "
        "  Console.WriteLine(r); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
