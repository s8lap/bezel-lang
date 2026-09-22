// Regression tests for issue 4494 — §22.5's declaration rule "Every parameter
// after the first defaulted parameter must itself have a default" was
// unenforced (§22.5 "Defaults and modifiers", v0.306, spec line 4711).
//
// THE SENTENCE, verbatim and in place — it closes §22.5's *Defaults and
// modifiers* paragraph:
//
//   "… `ref` and `out` parameters can never carry defaults — both require a
//    caller-side location, which an omitted argument cannot supply. Every
//    parameter after the first defaulted parameter must itself have a
//    default."
//
// MEASURED ON e9239ab4 (the parent of this fix):
//
//   class B(n: int := 41, s: string) …   `bezelc -c` exit 0, NO diagnostic.
//   `new B(1, "ab")` then ran and `b.Show()` returned 3.
//
// WHY IT MATTERS BEYOND TIDINESS. An accepted `(n: int := 41, s: string)`
// leaves a parameter that can NEVER be omitted sitting behind one that can, so
// the default is unreachable by any positional call — reachable only by
// naming, which is precisely the machinery §22.5 is about. The compiler's own
// §22.5 binder already DEPENDS on the rule: `sema::FormalsView::ofNames`
// represents a built-in row's optionality as a single "how many leading
// parameters are required" count and says so in as many words ("the registry's
// rows put every defaulted parameter LAST … §22.5 requires it"), a shape that
// cannot express an interleaved list at all.
//
// AND IT HAS ALREADY COST SOMETHING. A fix lane writing the #4484 regression
// test reached for `class B(n: int := 41, s: string)` as its "omitted middle
// slot" fixture; the compiler accepted it, so the shape LOOKED valid. The lane
// caught it by reading the spec and replaced the fixture with a legal one. An
// unenforced declaration rule silently invites test fixtures that are not
// valid Bezel, and a test built on an illegal declaration pins behaviour the
// language does not define.
//
// WHICH ROWS ARE THE PINS. The four `*_is_rejected_22_5` rows — a §22.10
// primary-constructor list (the issue's own repro), an ordinary method, a
// constructor, and a NESTED type's method (which pins that the check runs
// through the recursive nested-type walk and not only at depth 0). Each
// asserts the SPECIFIC §22.5 message rather than `!r.ran`, so a rejection
// arriving for an unrelated reason does not keep the row green.
//
// THE REST ARE CONTROLS. This fix is a NEW REJECTION, so the risk it carries
// is over-rejection: a list with a trailing run of defaults, a list with no
// defaults at all, and — the one a naive implementation breaks — a list whose
// LAST parameter is the only defaulted one.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// PIN 1 — the issue's own repro: a §22.10 PRIMARY-CONSTRUCTOR parameter list.
// ---------------------------------------------------------------------------

BEZEL_TEST(param_default_ordering_4494,
           primary_ctor_param_after_a_defaulted_one_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 41, s: string) "
        "  public method Show(): int return n + s.Length; end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b: B := new B(1, \"ab\"); "
        "    return b.Show(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "parameter `s` declares no default but follows defaulted "
           "parameter `n`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "(§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 2 — an ordinary METHOD parameter list. §22.5's sentence is not scoped to
// primary constructors, and the issue asks explicitly whether the ordinary
// lists share the gap. They did.
// ---------------------------------------------------------------------------

BEZEL_TEST(param_default_ordering_4494,
           method_param_after_a_defaulted_one_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public method Log(level: string := \"INFO\", message: string) "
        "    Console.WriteLine(level); "
        "    Console.WriteLine(message); "
        "  end Log; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(); "
        "    b.Log(\"W\", \"m\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "parameter `message` declares no default but follows defaulted "
           "parameter `level`"));
}

// ---------------------------------------------------------------------------
// PIN 3 — an explicit CONSTRUCTOR parameter list.
// ---------------------------------------------------------------------------

BEZEL_TEST(param_default_ordering_4494,
           constructor_param_after_a_defaulted_one_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public constructor(a: int := 3, b: int) "
        "    n := a + b; "
        "  end constructor; "
        "  public method Show() Console.WriteLine(n.ToString()); end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(1, 2); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "parameter `b` declares no default but follows defaulted "
           "parameter `a`"));
}

// ---------------------------------------------------------------------------
// PIN 4 — a NESTED type's method. §11.6 nests to any depth and the rule is a
// property of the declaration, not of its depth; this row is what makes the
// recursive walk load-bearing rather than incidental.
// ---------------------------------------------------------------------------

BEZEL_TEST(param_default_ordering_4494,
           nested_type_method_param_after_a_defaulted_one_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class Outer "
        "  public class Inner "
        "    public method M(a: int := 1, b: int) "
        "      Console.WriteLine((a + b).ToString()); "
        "    end M; "
        "  end Inner; "
        "end Outer; "
        "class C "
        "  public static method Main(): int "
        "    using var i := new Outer.Inner(); "
        "    i.M(1, 2); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "parameter `b` declares no default but follows defaulted "
           "parameter `a`"));
}

// ---------------------------------------------------------------------------
// CONTROL — a TRAILING RUN of defaults, the legal shape §22.5's own example
// uses (`Log(message: string, level: string := "INFO", timestamp: bool :=
// true)`). This is the shape the whole stdlib is written in; a rejection here
// would be catastrophic and silent in a check whose pins are all rejections.
// ---------------------------------------------------------------------------

BEZEL_TEST(param_default_ordering_4494,
           trailing_run_of_defaults_still_accepted_22_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public method Log(message: string, level: string := \"INFO\", "
        "                    loud: bool := true) "
        "    Console.WriteLine(message); "
        "    Console.WriteLine(level); "
        "    Console.WriteLine(loud.ToString()); "
        "  end Log; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(); "
        "    b.Log(\"started\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("started\nINFO\nTrue\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the ONE-defaulted-parameter-at-the-END shape, on a §22.10 primary
// constructor. An off-by-one in the "first defaulted" bookkeeping (comparing
// `firstDefaulted <= i` instead of `< i`) would reject the defaulted parameter
// against ITSELF, and every pin above would still pass.
// ---------------------------------------------------------------------------

BEZEL_TEST(param_default_ordering_4494,
           primary_ctor_with_only_a_trailing_default_still_accepted_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, k: int := 5) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(k.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(1); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n5\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a RECORD's positional field list. A.3 `PositionalField` admits no
// default syntax at all, so the rule is vacuous for records; this row exists
// because the walk still VISITS them (`TypeDecl::positionalFields` is the same
// vector for both kinds) and must find nothing to say.
// ---------------------------------------------------------------------------

BEZEL_TEST(param_default_ordering_4494,
           record_positional_fields_unaffected_22_5) {
    auto r = runAot(
        "module M; "
        "record Point(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Point := new Point(3, 4); "
        "    Console.WriteLine((p.X + p.Y).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
