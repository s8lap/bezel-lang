// Regression tests for issue #5089 — §5.1 "Nullable conversions" `S? → T?`
// LIFT at a GENERIC CONSTRUCTOR's `T?` FORMAL: `new Box<double>(a)` with
// `a: int?` and `class Box<T> ... constructor(x: T?) ...`.
//
// Sibling of #5046, which fixed this exact §5.1 `S? → T?` lift at a generic
// METHOD's `T?` formal (`emitGenericSpecialization` / `emitInlineGenericMethod`,
// both reached only through `emitMethodCall`). A generic CONSTRUCTOR's `T?`
// formal has the same shape of gap and was not touched by that fix: the
// constructor / `lowerNew` inline-ctor parameter-binding loop
// (LlvmBackend_Exprs2.cpp, "Bind constructor params to the concrete arg
// values") never routed its argument through `wrapArgForNullableValueFormal`
// at all — not the numeric-widen arm #5046 added, and not the original
// bare-payload / null-pointer arms #4830 added either. An argument that
// arrives already tagged as the caller's own `{i8, S}` struct at a DIFFERENT
// width/shape than the formal's `{i8, T}` (§5.1's `S? → T?` lift) fell
// through unwrapped: `verifyModule` rejected the mismatch once the
// specialized body actually UNWRAPS the parameter (matching the issue's own
// report: "PHI node operands are not the same type as the result!" over
// `x ?? -1.0`, `a: int?`, `Box<double>`).
//
// buildOrderedArgs's own per-argument nullable-wrap (fix #4967) does not
// catch this either: it lowers the constructor ARGUMENTS under the ENCLOSING
// (caller) generic substitution — deliberately, since the argument
// expressions belong to the caller's scope — so a bare `T?` formal does not
// yet resolve to a concrete nullable value type at that point. Only after
// `genericSubst_` is restored to THIS type's own substitution does
// `wrapArgForNullableValueFormal`'s generic-aware resolution
// (`nullableTypeParamConcrete` / `resolveTypeParamRef`, both reading the
// ACTIVE `genericSubstSemaTy_`) see the concrete formal — which is exactly
// the point the fix now calls it, mirroring `emitInlineGenericMethod`'s twin
// binding loop.
//
// EVERY row below unwraps `x` DIRECTLY inside the generic constructor body
// (`x ?? <default>` into a NON-nullable field, or `x!` for the record row)
// and prints the field from the CALLER's (non-generic) scope — proven, by
// running each pattern against a reverted build (LlvmBackend_Exprs2.cpp at
// HEAD~1, the pre-#5089 state), to be the shape that actually exercises the
// mismatch: an EARLIER draft of this file stored the raw `x ?? ...`-free
// parameter into a `T?` FIELD (`V := x;`) and printed `b.V ?? default` at
// the caller instead — 4 of its 5 rows passed even with the fix reverted,
// because a plain struct copy plus the CALLER's own coalesce on read
// doesn't force the specialized body itself to interpret `x`'s bits at the
// concrete width; only forcing the UNWRAP inside the still-generic body
// (where `x`'s declared type resolves through the active substitution to
// the concrete `T?`, mismatching what was actually stored) reproduces the
// issue's own verifier failure. `Console.WriteLine(x ?? ...)` called
// DIRECTLY inside the generic body was avoided (unlike the issue's own
// literal repro): probing found that shape hits an UNRELATED, pre-existing
// defect (`bz_string_to_utf8z` receiving a raw nullable-int struct — a
// WriteLine-argument-dispatch gap for a bare generic `T?` operand inside a
// template body, orthogonal to this ctor-binding-site fix), so every row
// here assigns into a plain field instead and prints from the caller.
//
// spec §5.1 "Implicit conversions" (int → double, sbyte → double, uint →
// long) and "Nullable conversions" ("S? implicitly converts to T? whenever S
// implicitly converts to T ... a present value converts by the underlying
// conversion, null converts to null"; "For any type T, T implicitly converts
// to T?"), §14.1 "Generic declarations", §22.7 "Constructors", v0.307.

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

// per §5.1 / §14.1 / §22.7 (v0.307), fix #5089 — the issue's repro (`int? ->
// double?` at a generic constructor's `T?` formal). `x ?? -1.0` inside the
// ctor body forces the coalesce to read `x`'s payload at the CONCRETE
// `double?` width; pre-fix, `x`'s slot still holds the caller's raw `int?`
// (`{i8, i32}`) bits, and `verifyModule` rejects the resulting PHI merge
// between an `i32` value and the `double` default (measured: "PHI node
// operands are not the same type as the result!", matching the issue).
// Present and null rows, each a separate `new Box<double>(...)` construction.
BEZEL_TEST(nullable_lift_generic_ctor_5089, int_nullable_lift_double_nullable_ctor_coalesce) {
    expectPrints(
        "module M; "
        "class Box<T> "
        "  public V: T; "
        "  public constructor(x: T?) V := x ?? -1.0; end constructor; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    var a: int? := 5; var z: int? := null; "
        "    var b := new Box<double>(a); "
        "    var c := new Box<double>(z); "
        "    Console.WriteLine(b.V); "
        "    Console.WriteLine(c.V); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "5\n-1\n");
}

// per §5.1 / §14.1 / §22.7 (v0.307), fix #5089 — an UNSIGNED source (`uint?`)
// widened through a generic constructor's `T?` formal bound to `long` must
// ZERO-extend, not sign-extend — mirroring #5046's review-correction row.
// `3000000000` does not fit a 32-bit signed int (its bit pattern is negative
// as `int`), so treating the source as signed would sign-extend that bit
// pattern into -1294967296 instead of the correct 3000000000. (Measured:
// this is the one row of the earlier field-passthrough draft that already
// failed pre-fix on its own, since the naive fallback interpretation
// defaults to sign-extension.)
BEZEL_TEST(nullable_lift_generic_ctor_5089, uint_nullable_lift_long_nullable_ctor_zero_extends) {
    expectPrints(
        "module M; "
        "class Box<T> "
        "  public V: T; "
        "  public constructor(x: T?) V := x ?? -1; end constructor; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    var u: uint? := 3000000000; var z: uint? := null; "
        "    var b := new Box<long>(u); "
        "    var c := new Box<long>(z); "
        "    Console.WriteLine(b.V); "
        "    Console.WriteLine(c.V); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "3000000000\n-1\n");
}

// per §5.1 / §14.1 / §22.7 (v0.307), fix #5089 — a SIGNED, NEGATIVE source
// (`sbyte?`) at a generic constructor's `T?` formal bound to `double`:
// combines a signed source (so a fix that hardcoded zero-extension would
// still get this row wrong) with a KIND change (int family to floating
// point, like the issue's own repro), which is what actually makes this row
// fail pre-fix. `sbyte? -> long?` (same int family, signed) was tried first
// and DID NOT distinguish pre/post fix: measured, it compiled and printed
// the correct value even on a reverted build, both under the plain harness
// and under BEZEL_AOT_FORCE_SANITIZE=address (`[san-scope] ... pass`) — a
// signed source's raw bit-pattern happens to already read back correctly
// under whatever fallback interpretation the unfixed path falls through to,
// so it would have been a vacuous row (see CLAUDE.md "a regression test
// must pin THE BUG, not the neighbourhood"). This row was measured to fail
// pre-fix with a genuine `verifyModule` rejection ("PHI node operands are
// not the same type as the result! %N = phi i8 [ %nv.val, %entry ],
// [ -7.000000e+00, %coal.rhs ]").
BEZEL_TEST(nullable_lift_generic_ctor_5089, sbyte_nullable_lift_double_nullable_ctor_sign_preserved) {
    expectPrints(
        "module M; "
        "class Box<T> "
        "  public V: T; "
        "  public constructor(x: T?) V := x ?? -7.0; end constructor; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    var s: sbyte? := -12; var z: sbyte? := null; "
        "    var b := new Box<double>(s); "
        "    var c := new Box<double>(z); "
        "    Console.WriteLine(b.V); "
        "    Console.WriteLine(c.V); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "-12\n-7\n");
}

// per §5.1 / §7.2 / §14.1 / §22.7 (v0.307), fix #5089 — a `null` LITERAL
// argument (not merely a `null`-valued variable) at a generic constructor's
// `T?` formal, immediately coalesced inside the ctor body. The literal
// lowers to a different unwrapped shape than the already-tagged-struct widen
// case above (matched via `nullableValueNull`); measured to reproduce a
// distinct pre-fix verifier failure ("PHI node operands are not the same
// type as the result! %N = phi ptr [...], [ -1.0, ... ]" — the untyped null
// marker merging against a `double` default).
BEZEL_TEST(nullable_lift_generic_ctor_5089, null_literal_ctor_arg) {
    expectPrints(
        "module M; "
        "class Box<T> "
        "  public V: T; "
        "  public constructor(x: T?) V := x ?? -1.0; end constructor; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    var b := new Box<double>(null); "
        "    Console.WriteLine(b.V); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "-1\n");
}

// per §5.1 "Nullable conversions" ("For any type T, T implicitly converts to
// T?") / §14.1 / §22.7 (v0.307), fix #5089 — a STRUCT (record) PAYLOAD: a
// bare, non-nullable record local passed to a generic constructor's `T?`
// formal must be WRAPPED into the tagged nullable struct (the bare-payload
// case `wrapArgForNullableValueFormal` already handled for a NON-generic /
// method formal since #4830), not merely left as the raw record struct.
// `x!` (null-forgiving unwrap) inside the ctor body forces the tag check at
// the concrete `Point?` shape; pre-fix, `x`'s slot holds only the raw
// `Point` payload with no tag byte at all, and `verifyModule` rejects the
// resulting tag comparison (measured: "Both operands to ICmp instruction are
// not of the same type! %has.val = icmp ne i32 %has.val8, i8 0"). `T` is
// bound to a non-numeric (record) type here, so this pins the OTHER arm of
// the helper (bare-payload wrap), not the numeric-widen arm #5046 added.
BEZEL_TEST(nullable_lift_generic_ctor_5089, record_payload_bare_wraps_to_nullable_ctor) {
    expectPrints(
        "module M; "
        "record Point(X: int, Y: int); "
        "class Box<T> "
        "  public V: T; "
        "  public constructor(x: T?) V := x!; end constructor; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    var p := new Point(3, 4); "
        "    var b := new Box<Point>(p); "
        "    Console.WriteLine(b.V.ToString()); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "Point(X: 3, Y: 4)\n");
}
