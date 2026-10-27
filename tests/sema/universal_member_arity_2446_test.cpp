// Regression tests for issue #2446: a wrong-arity call to a universal
// `Object` member (e.g. `.Equals()` with 0 args, `.GetHashCode(1)` with 1
// arg, `.ToString(x)`, …) compiled silently instead of producing an arity
// diagnostic.
//
// Root cause: the universal members `GetType`/`Equals`/`GetHashCode`/
// `ToString` (spec §8.1) plus `Dispose` (spec §8.7) are synthesised by
// codegen on every type and intentionally carry no `MethodSymbol`. Sema's
// ordinary arity machinery (`checkCallArity` / `checkArgsAgainstParams`)
// runs only when the callee resolved to a `MethodSymbol`, so a wrong-arity
// call on the universal surface had no diagnostic and codegen's #2434
// fallback degraded the result to a zero-initialized value.
//
// Fix (#2446, spec §8.1): in `typeCall`'s `!calleeSym` block — right after
// the existing `Console.Write` arity check — add a per-member arity check
// for the universal surface, gated to fire only when the receiver is an
// INSTANCE of a user-defined type or a bare generic type parameter. The
// guard skips:
//   * static-receiver calls (`char.ToString(c)`, `Convert.ToString(v)`,
//     `Object.ReferenceEquals(a, b)`) — the target IdentifierExpr resolves
//     to a TypeSymbol, not a value, and these stdlib static methods have
//     their own arity;
//   * primitive receivers (`string.Equals(s, ignoreCase)`,
//     `int.ToString(radix)`, …) and stdlib value-builtin receivers
//     (`DateTime.ToString(format)`, `decimal.ToString(radix)`, …) — these
//     types carry their OWN instance overloads of the universal-member
//     names whose arity differs from the §8.1 universal surface.
//
// Spec basis:
//   §8.1 Object — the universal members and their signatures:
//        GetType(): Type                       (0 args)
//        Equals(other: Object?): bool          (1 arg)
//        GetHashCode(): int                    (0 args)
//        ToString(): string                    (0 args)
//        ReferenceEquals(a, b): bool  (static) (2 args)
//   §8.7 `Dispose` is class-only, takes 0 args.
//   §11.3 member resolution.
//
// Each reject cell below MUST assert on the diagnostic TEXT (per the lane
// protocol, the coarse `hasErrors()` check is insufficient — `runAot`'s
// WIFSIGNALED path synthesises an `Error` for a runtime SIGSEGV too).

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// Run the full sema pipeline on the given source.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff any diagnostic message contains the given substring.
bool anyDiagContains(const bezel::test::SemaResult& r,
                     std::string_view substr) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(std::string(substr)) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// ===========================================================================
// Reject cells — wrong-arity calls on universal Object members must produce
// the §8.1 arity diagnostic. Each cell exercises a receiver-type axis the
// bug reproduced on.
// ===========================================================================

// §8.1 / §9.1: `.Equals()` (0 args) on a positional record receiver — the
// issue's exact repro shape. Without the fix this compiled silently and
// `b` evaluated to false at runtime.
BEZEL_TEST(sema_universal_member_arity_2446, record_equals_zero_args_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(9, 5); "
        "  var b := p.Equals(); "
        "  return (if b then 1 else 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "universal `Object.Equals`"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "1 argument(s)"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "supplies 0"));
}

// §8.1 / §8.3: `.GetHashCode(1)` (1 arg) on a record receiver — universal
// GetHashCode takes 0 args.
BEZEL_TEST(sema_universal_member_arity_2446, record_gethashcode_one_arg_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(9, 5); "
        "  var h := p.GetHashCode(1); "
        "  return h; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "universal `Object.GetHashCode`"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "0 argument(s)"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "supplies 1"));
}

// §8.1 / §8.5: `.ToString(1)` (1 arg) on a record receiver — universal
// ToString takes 0 args.
BEZEL_TEST(sema_universal_member_arity_2446, record_tostring_one_arg_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(9, 5); "
        "  var s := p.ToString(1); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "universal `Object.ToString`"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "supplies 1"));
}

// §8.1 / §35: choice receiver — same wrong-arity diagnostic on a value-like
// choice (whose case payload is an admissible equality component).
BEZEL_TEST(sema_universal_member_arity_2446, choice_equals_zero_args_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "public choice Shape Circle(r: double), Square(s: double); end Shape; "
        "class C public static method Main(): int "
        "  var sh := Shape.Circle(1.0); "
        "  var b := sh.Equals(); "
        "  return (if b then 1 else 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "universal `Object.Equals`"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "supplies 0"));
}

// §8.1 / §11: class receiver — a non-static user class with no user Equals
// override hits the universal Object.Equals surface; 0 args is wrong.
BEZEL_TEST(sema_universal_member_arity_2446, class_equals_zero_args_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class D public constructor() end; end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  var b := d.Equals(); "
        "  return (if b then 1 else 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "universal `Object.Equals`"));
}

// §8.1 / §11: interface-typed receiver — even though interface member
// resolution is incomplete in sema, the universal surface is present on
// every type, so the arity check must still fire.
BEZEL_TEST(sema_universal_member_arity_2446, interface_equals_zero_args_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "interface I method Foo(): int; end; "
        "class D : I public method Foo(): int return 7; end Foo; end D; "
        "class C public static method Main(): int "
        "  var i: I := new D(); "
        "  var b := i.Equals(); "
        "  return (if b then 1 else 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "universal `Object.Equals`"));
}

// §8.1 / §14.2: a bare generic type parameter `T` admits the universal
// Object surface (the existing `isObjectMember` exemption in
// resolveMemberAccess's bare-T block documents this). 0 args on `Equals`
// is still wrong arity.
BEZEL_TEST(sema_universal_member_arity_2446, bare_typeparam_equals_zero_args_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Id<T>(v: T): T "
        "  var b := v.Equals(); "
        "  return v; "
        "end Id; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "universal `Object.Equals`"));
}

// §8.7: `Dispose` is class-only and takes 0 args; 1 arg is wrong.
BEZEL_TEST(sema_universal_member_arity_2446, class_dispose_one_arg_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class D public constructor() end; end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  d.Dispose(1); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "universal `Object.Dispose`"));
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "supplies 1"));
}

// §7.5 / §8.1: the null-conditional form `obj?.Equals()` shares the same
// no-symbol path; the arity check applies there too.
BEZEL_TEST(sema_universal_member_arity_2446, nullcond_equals_zero_args_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p: Pt? := new Pt(9, 5); "
        "  var b := p?.Equals(); "
        "  return (if b then 1 else 2); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "universal `Object.Equals`"));
}

// ===========================================================================
// Accept cells — correct-arity universal member calls must NOT be falsely
// rejected (the false-reject axis the arity check could break).
// ===========================================================================

// §8.1: correct-arity calls on a record receiver compile cleanly.
BEZEL_TEST(sema_universal_member_arity_2446, record_correct_arity_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(9, 5); "
        "  var q := new Pt(9, 5); "
        "  var b := p.Equals(q); "
        "  var h := p.GetHashCode(); "
        "  var s := p.ToString(); "
        "  return (if b then 1 else 2); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.1: correct-arity calls on a class with user overrides resolve to the
// user MethodSymbol (calleeSym non-null), bypass the universal-arity check
// entirely, and compile cleanly.
BEZEL_TEST(sema_universal_member_arity_2446, user_override_correct_arity_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class D "
        "  public override method Equals(other: D): bool return true; end Equals; "
        "  public override method GetHashCode(): int return 42; end GetHashCode; "
        "  public override method ToString(): string return \"D\"; end ToString; "
        "end D; "
        "class C public static method Main(): int "
        "  var d := new D(); "
        "  var b := d.Equals(d); "
        "  var h := d.GetHashCode(); "
        "  var s := d.ToString(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.1: correct-arity calls on a value-like choice receiver.
BEZEL_TEST(sema_universal_member_arity_2446, choice_correct_arity_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "public choice Shape Circle(r: double), Square(s: double); end Shape; "
        "class C public static method Main(): int "
        "  var sh := Shape.Circle(1.0); "
        "  var b := sh.Equals(sh); "
        "  var h := sh.GetHashCode(); "
        "  var s := sh.ToString(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.1 / §9.5 / §2.18: a static call on a primitive type name that shares the
// universal-member name (`char.ToString('z')`) must NOT be falsely rejected by
// the UNIVERSAL-ARITY check — the receiver is a type-name IdentifierExpr (a
// TypeSymbol), not an instance value, so that check is suppressed.
//
// fix-forward (campaign 2's anchor): the call IS rejected, but by the §22.6
// ADMISSION gate, not by the universal-arity check. Stdlib §9.5 declares
// `ToString(): string` and §2.18's vocabulary for `char` is empty, so the
// `ToString(format: string)` FORM (which fix #7333/#7336 put back into the
// built-in parameter table) takes a FORMAT STRING — and a `char` argument
// reaches no candidate. The row therefore asserts the §22.6 diagnostic: a
// §8.1 arity diagnostic here would be the false rejection this row exists to
// catch. (RENAMED from `char_static_tostring_accepted`; the lit twin is
// `tests/lit/sema/sema_unknown_primitive_method_spec3_4__char_tostring_with_an_argument_is_rejected.bzl`.)
BEZEL_TEST(sema_universal_member_arity_2446,
           char_static_tostring_with_an_argument_is_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var s := char.ToString('z'); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(
        r, "no matching overload of `char.ToString`"));
}

// §8.1 / §2.3: a static call on a builtin class name that shares the
// universal-member name (`Convert.ToString(value: int)`,
// `Convert.ToString(value: int, toBase: int)`) must NOT be falsely
// rejected.
BEZEL_TEST(sema_universal_member_arity_2446, convert_static_tostring_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var s := Convert.ToString(5); "
        "  var t := Convert.ToString(5, 16); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.1 / §9.6: a primitive (string) instance call carrying a 2-arg
// `Equals(other, ignoreCase)` overload must NOT be falsely rejected —
// primitives carry their own instance overloads whose arity differs from
// the §8.1 universal surface.
BEZEL_TEST(sema_universal_member_arity_2446, string_equals_two_arg_overload_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var b := s.Equals(\"world\", false); "
        "  return (if b then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.1 / §2.8: a value-builtin (DateTime) instance call carrying a 1-arg
// `ToString(format)` overload must NOT be falsely rejected — value-builtins
// carry their own instance overloads whose arity differs from the §8.1
// universal surface.
BEZEL_TEST(sema_universal_member_arity_2446, datetime_tostring_format_overload_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C public static method Main(): int "
        "  var dt: DateTime := DateTime.MinValue; "
        "  var s := dt.ToString(\"yyyy\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
