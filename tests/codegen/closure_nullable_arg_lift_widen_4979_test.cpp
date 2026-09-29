// Regression tests for issue #4979 — §5.1 "Nullable conversions" `S? → T?`
// at a CLOSURE formal and at the `?.` (null-safe method-call) form.
//
// #4967 (8e8ff952) added the widening to buildOrderedArgs and the omitted-
// default fillOmittedParamDefaults, but two sibling arms in
// LlvmBackend_Calls2.cpp were left out: the closure-call dispatch arm
// (the `isNullableValueTy(formalTy)` wrap around the closure's `S? → T?`
// lift) and the imported-callee `?.` arm. Both reach the call as the
// already-tagged `{i8, S}` struct against a `{i8, T}` formal — IR
// verification failure ("Call parameter type does not match function
// signature!") for any narrower numeric `S?` at a wider `T?` formal.
//
// spec §5.1 "Implicit conversions" (int → long, float → double, uint →
// ulong) and "Nullable conversions" (the rules compose through at most
// one `?` on each side); §22.6 stage 3, v0.307.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §5.1 / §22.6 (v0.307), fix #4979 — the issue's repro verbatim: an
// `int?` argument bound to a closure's `long?` formal, present and null.
// `Id<long>(a)` (the direct-call twin #4967 fixed) returns `7` on the same
// source; the closure path was the unfixed sibling.
BEZEL_TEST(closure_nullable_arg_lift_widen_4979,
           int_nullable_at_closure_long_nullable_formal) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method Id<T>(x: T?): T where T: value "
        "    return x ?? default(T); "
        "  end Id; "
        "  public static method Main(): int "
        "    var f := lambda(x: long?): long => x ?? -3; "
        "    var a: int? := 7; var z: int? := null; "
        "    Console.WriteLine(f(a)); "
        "    Console.WriteLine(f(z)); "
        "    Console.WriteLine(Id<long>(a)); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "7\n-3\n7\n");
}

// per §5.1 (v0.307), fix #4979 — sibling widening `byte? → int?` at a
// closure's `int?` formal, present and null. Distinct type widths so a
// fix that hard-codes int→long would still pass this row.
BEZEL_TEST(closure_nullable_arg_lift_widen_4979,
           byte_nullable_at_closure_int_nullable_formal) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(x: int?): int => x ?? -11; "
        "    var b: byte? := 9; var bn: byte? := null; "
        "    Console.WriteLine(f(b)); "
        "    Console.WriteLine(f(bn)); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "9\n-11\n");
}

// per §5.1 (v0.307), fix #4979 — a closure with NO widening still has to
// pass the null tagged struct (the literal `null` becomes `{i8 0, T 0}`,
// the present `S?` arrives as `{i8 1, S}`). Regression pin: a fix that
// unconditionally re-wraps via the widen arm would mis-tag the hasValue
// byte (or skip the widening path entirely if the shapes match).
BEZEL_TEST(closure_nullable_arg_lift_widen_4979,
           same_width_int_nullable_at_closure_int_nullable_passes_through) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(x: int?): int => x ?? -2; "
        "    var p: int? := 42; var n: int? := null; "
        "    Console.WriteLine(f(p)); "
        "    Console.WriteLine(f(n)); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "42\n-2\n");
}

// per §5.1 (v0.307), fix #4979 — `uint? → long?` at a closure's `long?`
// formal: zero-extends (the alternative sign-extension would print a huge
// number — the classic unsigned-vs-signed bug). Pins the
// `isUnsignedTargetExpr` direction the closure path now forwards.
BEZEL_TEST(closure_nullable_arg_lift_widen_4979,
           uint_nullable_at_closure_long_nullable_zero_extends) {
    expectPrints(
        "module M; "
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(x: long?): long => x ?? -1; "
        "    var u: uint? := 4000000000; "
        "    Console.WriteLine(f(u)); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "4000000000\n");
}