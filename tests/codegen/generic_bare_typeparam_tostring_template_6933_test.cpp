// Regression test for issue #6933 (§14.1 / §14.2 / §8.1 / §8.5, v0.357).
//
// `x.ToString()` on a bare `T` receiver inside a generic method's UNSPECIALIZED
// template body had NO lowering at all. Every generic declaration is emitted
// once unspecialized, with its type parameters erased to the `int` (i32)
// placeholder; inside that body there is no active substitution
// (`genericSubstSemaTy_` is empty), so neither of the two ToString redirects
// could resolve the receiver — `tryLowerValueSemanticsCall`'s #1091 / #2469
// arms and the `if (!ms)` fallback's own primitive-substitution branch both
// need a substitution. The call therefore fell through to the end of the
// dispatch chain, reported
//
//     internal: codegen cannot dispatch member 'ToString' on receiver of
//     type 'T': permissive acceptance (spec #3 / issue #605) … (#2477)
//
// — a FALSE report: §8.1 declares `ToString` on the universal Object surface
// and §14.2 gives every `T` the Object members, so the member EXISTS — the
// compiler simply had no dispatch for it there — and returned null. At a
// CALL-ARGUMENT position that null became
//
//     internal: lowerExpr returned null for call argument in buildOrderedArgs
//     — #1386
//
// an internal ERROR, and the compile of a legal program STOPPED.
//
// THE TEMPLATE IS NOT DEAD. An importing module binds `Gen<int>` to the
// exported template — `int` being the only binding the #1943 / #6779 / #6818
// gates let cross the module boundary — so the i32 placeholder IS the `int`
// the template carries there, and the template's `x.ToString()` must render it
// as the `int` it is. The sibling §8.1 member on the same shape
// (`x.GetHashCode()`) was fixed by #6907; this is its ToString twin. The
// cross-module rows live in
// `tests/module/xmod_generic_method_tostring_6933_test.cpp`, which pins the
// VALUE the live template computes.
//
// EVERY VALUE BELOW IS ASSERTED, NOT "did not crash": the expected output is
// the §8.5 decimal rendering of the `int` placeholder, so `5` renders as "5"
// and its length is 1.

#include <string>

#include "support/aot_support.hpp"  // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The `U.Record` sink the issue's own repro uses.
const char* kSink =
    "public static class U "
    "  public static method Record(n: int) Console.WriteLine($\"rec {n}\"); "
    "end Record; "
    "end U; ";

std::string out(const JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO — a generic METHOD's own `T`, as a CALL ARGUMENT.
// ---------------------------------------------------------------------------

// Issue #6933's repro program, verbatim (module name and all). §14.2: the
// program is legal — every `T` has the Object members, so `x.ToString()` is a
// well-typed call — and §8.5 fixes the `int` placeholder's rendering as its
// decimal digits, so `5` renders as "5" and its length is 1.
//
// BEFORE THE FIX: the compile STOPPED on the #1386 internal error, so the
// program never ran at all (this is the issue's "a legal program fails to
// compile (internal error)").
BEZEL_TEST(generic_bare_typeparam_tostring_template_6933,
           issue_repro_call_argument_compiles_and_records_the_length) {
    const std::string program =
        "module MB7argS; "
        "public static class U "
        "  public static method Record(n: int) Console.WriteLine($\"rec {n}\"); "
        "end Record; "
        "end U; "
        "public class Prog "
        "  static method Gen<T>(x: T) "
        "    U.Record(x.ToString().Length); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var a := 5; "
        "    Gen(a); "
        "    return 0; "
        "  end Main; "
        "end Prog;";
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);   // before the fix: the #1386 compile error
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), "rec 1\n");
}

// The same shape on a generic CLASS's own type parameter (the erased template
// of `Holder<T>`'s method). The class's `T` erases to the same i32
// placeholder, so the same dispatch is required — and before the fix this
// shape stopped the compile with the same #1386 error.
BEZEL_TEST(generic_bare_typeparam_tostring_template_6933,
           generic_class_type_parameter_call_argument_compiles) {
    const std::string program =
        "module M; "
        "public static class U "
        "  public static method Record(n: int) Console.WriteLine($\"rec {n}\"); "
        "end Record; "
        "end U; "
        "public class Holder<T> "
        "  private _x: T; "
        "  public constructor(x: T) _x := x; end constructor; "
        "  public method M() U.Record(_x.ToString().Length); end M; "
        "  destructor end destructor; "
        "end Holder; "
        "public class Prog "
        "  public static method Main(): int "
        "    using var h := new Holder<int>(5); "
        "    h.M(); "
        "    return 0; "
        "  end Main; "
        "end Prog;";
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), "rec 1\n");
}

// ---------------------------------------------------------------------------
// CONTROLS — the shapes this fix must NOT change (green on BOTH arms).
// ---------------------------------------------------------------------------

// A CONCRETE `int` receiver: `a.ToString()` was already dispatched through
// `emitPrimitiveToString` by the non-generic path. The new arm is gated on a
// bare type parameter inside the erased template, so this must be untouched —
// it is the admitting twin of the rows above (the same shape with a legal,
// already-dispatched value).
BEZEL_TEST(generic_bare_typeparam_tostring_template_6933,
           control_concrete_int_receiver_records_the_length) {
    const std::string program =
        "module M; " +
        std::string(kSink) +
        "public class Prog "
        "  public static method Main(): int "
        "    var a := 5; "
        "    U.Record(a.ToString().Length); "
        "    return 0; "
        "  end Main; "
        "end Prog;";
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), "rec 1\n");
}

// The RETURN position of the same generic shape. The same-module call runs a
// per-call-site specialization (§14.1), so the VALUE was already right before
// the fix — this row pins that the fix does not change it. (What the fix
// changes here is that the ERASED TEMPLATE stops degrading its own copy to a
// zero/null value and stops reporting the false "cannot dispatch member"
// line; the cross-module file pins the value that the erased template
// computes.)
BEZEL_TEST(generic_bare_typeparam_tostring_template_6933,
           control_return_position_records_the_length) {
    const std::string program =
        "module M; " +
        std::string(kSink) +
        "public class Prog "
        "  static method Gen<T>(x: T): int "
        "    return x.ToString().Length; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var a := 5; "
        "    U.Record(Gen(a)); "
        "    return 0; "
        "  end Main; "
        "end Prog;";
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), "rec 1\n");
}

// A CONCRETE `string` receiver: `s.ToString()` is the §8.5 identity, so the
// length is 3 — a different value from the `int` rows, which is what makes it
// a control rather than a duplicate of them.
BEZEL_TEST(generic_bare_typeparam_tostring_template_6933,
           control_concrete_string_receiver_records_the_length) {
    const std::string program =
        "module M; " +
        std::string(kSink) +
        "public class Prog "
        "  public static method Main(): int "
        "    var s := \"abc\"; "
        "    U.Record(s.ToString().Length); "
        "    return 0; "
        "  end Main; "
        "end Prog;";
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), "rec 3\n");
}
