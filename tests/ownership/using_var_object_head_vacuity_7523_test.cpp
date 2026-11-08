// Spec-derived regression for issue #7523 (and its sibling #7922): the §28.10
// "vacuous `using var`" lint must NOT fire on a binding whose RIGHT-HAND SIDE
// is declared `Object`.
//
// THE DEFECT
// ==========
// The lint (`MethodOwnership::handleLocalDecl`, src/ownership/Ownership.cpp)
// fires the warning
//
//   "`using var` `x` is vacuous — the RHS is a value-typed/runtime-managed
//    value with no disposal obligation (§28.9); use `const x := …;` …"
//
// whenever the right-hand side's STATIC TYPE is not a borrow, not a disposable
// class type, not an owning choice and not a lazy query. That is a set of
// NEGATIONS, so every type it does not name fell through into "value-typed/
// runtime-managed" — a DEFAULT, not a classification.
//
// `Object` is not named by any of them, so an `Object`-declared binding was
// classified as "value-typed/runtime-managed". Its premise is FALSE:
//
//   spec §28.9 (v0.350): the runtime-managed tier is `string`, `Type`, boxed
//     value types and function values — "None of them is a class", and "This
//     tier is exhaustive". `Object` is not on it.
//   spec §8.1 / §4: `Object` is the ONE primitive that may hold a class
//     instance, so an `Object`-declared binding may hold a class with a
//     destructor.
//   spec §28.12 (v0.350): "`await using` selects the logical-disposal path by
//     the class of the object the binding holds … never by the binding's
//     static type." The static type decides NOTHING about which cleanup runs.
//   spec §30.7 (v0.355, conformance case s30_7_0009): of an `Object`-static-
//     type `await using var`, "neither an interface nor `Object` can promise a
//     destructor, and the disposal path is the run-time object's, so the
//     runtime type's fallback posture is statically unknowable".
//
// So for an `Object`-declared right-hand side the answer is UNKNOWN, not "no
// obligation" — and a lint that claims a knowledge it does not have must stay
// quiet, exactly as the sibling Case B classifier does ("Conservative:
// returns false when the source cannot be classified, so the lint stays quiet
// rather than wrong").
//
// §28.9's "The compiler does not warn about undisposed runtime-managed values"
// has NO converse sentence: nothing in the spec licenses a vacuity claim about
// a binding that is not on the runtime-managed tier.
//
// THE ORACLE (and why these absence rows are not vacuous)
// =======================================================
// `runAot` fills `JitResult::diagnostics` only on a REJECT (#3138), so
// asserting the ABSENCE of a Warning on a program that COMPILES would be
// vacuous. These rows therefore use `parseAndCheckOwnership`, the in-process
// ownership-pass oracle, which reports the pass's WARNINGS for a compiling
// program — the same oracle the §28.10 rows of #2695 / #7456 use.
//
// The absence rows are non-vacuous because the CONTROL rows below assert that
// the SAME lint, with the SAME `Object` head, DOES fire when the value bound
// is provably on §28.9's tier (a boxed value, a string): the oracle can see
// the warning, so its absence is a fact about the compiler, not about the
// oracle. A fix that silenced the lint wholesale fails the control rows.
//
// #7922 is the SAME defect: the parenthesised spelling `using var x := (s);`.
// The parser keeps NO ParenExpr node ("no ParenExpr node survives into the
// AST", src/parser/Parser_Exprs.cpp), so `(s)` and `s` are the same operand
// shape and the same fix covers both.
//
// per §28.9, §28.10, §28.12, §30.7, §8.1, §4, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// True iff some WARNING diagnostic fires the inert-RHS vacuity message for
// `name` — "`using var` `x` is vacuous — the RHS is a value-typed/
// runtime-managed value with no disposal obligation (§28.9)". Matching the
// RULE-CITING FRAGMENT as well as "is vacuous" rules out Case B's
// borrow-pin warning, which is a DIFFERENT lint (and must keep firing).
bool hasInertRhsWarning(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("RHS is a value-typed/runtime-managed") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff some WARNING diagnostic fires the vacuous-BORROW-PIN message for
// `name` — "`using var` `x` pins a source that already outlives this scope".
// This is Case B of the same lint: the OTHER lint, which #7523 must not
// remove. It is the right one for a PARAMETER source (the parameter outlives
// every body-local scope, so the pin extends nothing — §28.10).
bool hasVacuousBorrowPinWarning(const OwnershipResult& r,
                                const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("pins a source that already outlives") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff some ERROR diagnostic fires §28.10's adoption rejection — the
// unmarked `using var` over a named OWNED binding whose declared type is a
// class or interface type (`Object` included, §8.1).
bool hasAdoptionAmbiguityError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("is ambiguous") == std::string::npos) continue;
        return true;
    }
    return false;
}

// The class with a destructor the `Object` head holds.
const char* kRes =
    "public class Res "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"DTOR\"); end destructor; "
    "end Res; ";

// The COPY form: an `Object`-declared source that is a BORROWING ALIAS of the
// owned `s` (`var src: Object := s;` is an unmarked read of an owned binding,
// which is a borrow — §28.3), so the `using var` over it is a borrow-pin and
// §28.10's adoption rejection does not fire. This is the shape the issue
// lists as `copy_local_source`.
std::string copyLocalSourceProgram(const std::string& head) {
    return std::string("module M; ") + kRes +
           "public class Program "
           "  public static method Main() "
           "    var s: Object := new Res(); "
           "    var src: Object := s; "
           "    using var x := src; "
           "    Console.WriteLine(\"END\"); "
           "  end Main; "
           "end Program; ";
}

// The same program with the `Object` head ANNOTATED on the binding under
// test — the issue's own `using var` spelling.
std::string annotatedProgram(const std::string& head) {
    return std::string("module M; ") + kRes +
           "public class Program "
           "  public static method Main() "
           "    var s: Object := new Res(); "
           "    var src: Object := s; "
           "    using var x: " + head + " := src; "
           "    Console.WriteLine(\"END\"); "
           "  end Main; "
           "end Program; ";
}

// A `const` source — the issue's `copy_const_source` spelling.
std::string constSourceProgram() {
    return std::string("module M; ") + kRes +
           "public class Program "
           "  public static method Main() "
           "    var s: Object := new Res(); "
           "    const src: Object := s; "
           "    using var x := src; "
           "    Console.WriteLine(\"END\"); "
           "  end Main; "
           "end Program; ";
}

// A PARAMETER source — the issue's `copy_param_source` spelling. The
// parameter is a borrow, so the callee's `using var x := src;` is a
// borrow-pin; the parameter outlives the callee's scope, so Case B (the
// borrow-pin lint) is the one that applies.
std::string paramSourceProgram() {
    return std::string("module M; ") + kRes +
           "public class Util "
           "  public static method Take(src: Object) "
           "    using var x := src; "
           "  end Take; "
           "end Util; "
           "public class Program "
           "  public static method Main() "
           "    var s: Object := new Res(); "
           "    Util.Take(s); "
           "    Console.WriteLine(\"END\"); "
           "  end Main; "
           "end Program; ";
}

// The NULLABLE `Object` head — the same shape with `Object?`.
std::string nullableHeadProgram() {
    return std::string("module M; ") + kRes +
           "public class Program "
           "  public static method Main() "
           "    var s: Object? := new Res(); "
           "    var src: Object? := s; "
           "    using var x: Object? := src; "
           "    Console.WriteLine(\"END\"); "
           "  end Main; "
           "end Program; ";
}

// A CLASS-VALUED source bound at the `Object` head: the value the binding
// holds is a class instance with a destructor, so "the RHS is a value-typed/
// runtime-managed value" is false of it twice over (the value is a class, and
// the slot's static type is `Object`).
std::string classValuedSourceProgram() {
    return std::string("module M; ") + kRes +
           "public class Program "
           "  public static method Main() "
           "    var s := new Res(); "
           "    var src := s; "
           "    using var x: Object := src; "
           "    Console.WriteLine(\"END\"); "
           "  end Main; "
           "end Program; ";
}

// The issue's LITERAL repro: the `using var` is declared at `Object` over an
// OWNED `Object` binding. §28.10's adoption rejection fires (the binding is
// in owned state and `Object` is a class-or-interface declared type, §8.1) —
// and the vacuity warning must NOT be drawn on top of it, because its premise
// is false there too.
std::string ownedSourceProgram() {
    return std::string("module M; ") + kRes +
           "public class Program "
           "  public static method Main() "
           "    var s: Object := new Res(); "
           "    using var x: Object := s; "
           "    Console.WriteLine(\"END\"); "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// ============================================================================
// THE FIXED SHAPES — the §28.9 vacuity warning must NOT be drawn.
//
// Every row below FAILS WITHOUT THE FIX: on the pre-fix tree the compiler
// classified the `Object` head as "value-typed/runtime-managed" and drew the
// warning on each of these programs.
// ============================================================================

// per §28.9 / §28.10 / §28.12, fix #7523
BEZEL_TEST(using_var_object_head_vacuity_7523,
            object_declared_source_does_not_draw_the_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(copyLocalSourceProgram("Object"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
}

// per §28.9 / §28.10 / §28.12, fix #7523
BEZEL_TEST(using_var_object_head_vacuity_7523,
            object_annotated_head_does_not_draw_the_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(annotatedProgram("Object"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
}

// per §28.9 / §28.10 / §28.12, fix #7523
BEZEL_TEST(using_var_object_head_vacuity_7523,
            object_const_source_does_not_draw_the_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(constSourceProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
}

// per §28.9 / §28.10 / §28.12, fix #7523
//
// The PARAMETER source is the shape where the OTHER lint of the same family
// applies: the parameter outlives the callee's scope, so the pin extends
// nothing and Case B ("pins a source that already outlives this scope") is
// the true statement. The §28.9 vacuity warning must not be drawn INSTEAD of
// it — and Case B must not be lost.
BEZEL_TEST(using_var_object_head_vacuity_7523,
            object_parameter_source_does_not_draw_the_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(paramSourceProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_TRUE(hasVacuousBorrowPinWarning(r, "x"));
}

// per §28.9 / §28.10 / §28.12, fix #7523
BEZEL_TEST(using_var_object_head_vacuity_7523,
            nullable_object_head_does_not_draw_the_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(nullableHeadProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
}

// per §28.9 / §28.10 / §28.12, fix #7523
//
// A CLASS-VALUED source bound at the `Object` head: the value is a class
// instance with a destructor, so the premise is false.
BEZEL_TEST(using_var_object_head_vacuity_7523,
            object_head_over_a_class_valued_source_does_not_warn_28_10) {
    auto r = parseAndCheckOwnership(classValuedSourceProgram());
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
}

// per §28.9 / §28.10, fix #7523
//
// The issue's literal repro. The §28.10 ADOPTION rejection is the verdict
// here (the source is in owned state), and the row pins BOTH: the rejection
// is there, and the false §28.9 vacuity warning is NOT drawn on top of it.
BEZEL_TEST(using_var_object_head_vacuity_7523,
            the_issue_repro_is_rejected_for_adoption_and_draws_no_vacuity_warning_28_10) {
    auto r = parseAndCheckOwnership(ownedSourceProgram());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAdoptionAmbiguityError(r));
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
}

// ============================================================================
// THE CONTROLS — the lint must NOT be silenced wholesale.
//
// These rows are what make the absence rows above non-vacuous: the SAME lint,
// with the SAME `Object` head, over a value that IS provably on §28.9's tier.
// ============================================================================

// per §28.9 / §28.10, fix #7523
//
// An `Object`-declared binding over a BOXED VALUE (§8.6): a boxed value type
// IS one of §28.9's runtime-managed kinds, so the premise holds and the
// warning must still be drawn. The `Object` head alone does not silence the
// lint — the VALUE does.
BEZEL_TEST(using_var_object_head_vacuity_7523,
            control_object_head_over_a_boxed_value_still_warns_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Program "
        "  public static method Main() "
        "    using var x: Object := 5; "
        "    Console.WriteLine(\"END\"); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

// per §28.9 / §28.10, fix #7523
//
// A runtime-managed STRING at an `Object` head: `string` IS one of §28.9's
// runtime-managed kinds, so the warning still fires.
BEZEL_TEST(using_var_object_head_vacuity_7523,
            control_object_head_over_a_string_still_warns_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Program "
        "  public static method Main() "
        "    using var x: Object := \"s\"; "
        "    Console.WriteLine(\"END\"); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

// per §28.9 / §28.10, fix #7523
//
// The plain value-typed spelling, unchanged by the fix.
BEZEL_TEST(using_var_object_head_vacuity_7523,
            control_value_typed_using_var_still_warns_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Program "
        "  public static method Main() "
        "    using var x := 5; "
        "    Console.WriteLine(\"END\"); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

// ============================================================================
// THE RUNTIME ORACLE — the lint's silence is not "there is nothing to
// dispose": the object the `Object` head holds is a class instance, and its
// destructor runs exactly once, at the OWNER's scope end (§28.3 / §28.4).
// ============================================================================

// per §28.3 / §28.4 / §28.10 / §28.12, fix #7523
BEZEL_TEST(using_var_object_head_vacuity_7523,
            the_object_head_binding_still_disposes_the_class_instance_once_28_4) {
    JitResult r = runAotChecked(copyLocalSourceProgram("Object"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "END\nDTOR\n");
}
