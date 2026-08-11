// Spec-derived regression tests for issue #2986 — foldIntConstExprTC folded
// BARE identifiers only (§17.14 narrowing), so a QUALIFIED class const
// reference `C.GK` was NOT a §17.14 compile-time integer constant and was
// rejected with the §5.1 narrowing error even when its value plainly fit the
// target type. The class-level const-initializer path (TypeResolution's
// checkTypeConsts, #1095 / §22.12) already folded `K.V + 1`; this fix folds
// the same shape when the value drives §5.1 narrowing at the typecheck pass.
//
// §17.14 (v0.184, #1688, *Constant expressions*): verbatim cites the
// qualifying ref:
//   "A constant expression is a literal of primitive type, a named const
//    whose declared type is an integer-family primitive (or `string`), or
//    an operation on constant expressions with the operators `+ - * / mod
//    << >> & | ^ ~`. A `const K := C.GK;` declaration is the same shape
//    (`C.GK` is a const-init qualifier, §22.12 / #1095); the typecheck
//    narrowing pass must fold it for the §5.1 rule to fire."
// §5.1 (v0.240, *Constant narrowing*) admits `var b: byte := E;` when E folds
// to a value in byte's range. The defect: when E is `C.GK`, foldIntConstExprTC
// returned nullopt (the bare-name resolver had no path through a class-member
// const) and the §5.1 narrowing produced a false reject.
//
// REGRESSION-TEST POLICY (regression-test-pins-the-bug): every PASS case below
// was REJECTED before the fix with "the initializer for `b` has a type that
// is not implicitly convertible to the target type (§5.1)". Every REJECT case
// here passes both before and after the fix by design — those tests pin the
// fix did not lift acceptance past §17.14 / §5.1 (an out-of-range qualified
// const, a non-const static, and an unreachable head still false-reject).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// AOT runner: compiles a program to native, runs it, returns its exit code.
// Returns -999 if the compile/link/run pipeline failed (so an "exit 0" test
// will not pass by accident when the compiler itself rejected the program).
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Sema-only runner: parse + collect + resolve types + typecheck expressions,
// returning the diagnostics. Used by the REJECT half of the file (the fix
// lifts an acceptance, so the load-bearing half is the rejection guard).
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// PASS — the defect #2986 reported. Each of these was REJECTED before the
// fix with "the initializer … has a type that is not implicitly convertible".
// ---------------------------------------------------------------------------

// §17.14 / §5.1 (#2986): the issue's exact shape. `var b: byte := C.GK;`
// where `GK` is a class const of type `int` with value `42`. The fix folds
// `C.GK` to 42 via the qualified-name arm; `fitsInIntType(42, byte)` is true;
// the §5.1 narrowing accepts. Verified end-to-end beyond type-check: the
// program compiles, links, returns 42.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           var_byte_narrows_from_qualified_const_spec_5_1_17_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public const GK: int := 42; "
        "  public static method Main(): int "
        "    var b: byte := C.GK; "
        "    if b = 42 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §17.14 / §5.1 (#2986): the same shape with `byte` as the source type
// (no narrowing actually needed, but the qualified path still exercises the
// new arm — `fitsInIntType` is vacuous but the result type IS the byte type).
// Verified end-to-end by returning 7 unchanged.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           var_byte_keeps_qualified_byte_const_spec_5_1_17_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public const GK: byte := 7; "
        "  public static method Main(): int "
        "    var b: byte := C.GK; "
        "    if b = 7 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §17.14 / §5.1 (#2986): a NEGATIVE int value (-42) narrows into `sbyte`.
// The fold resolves the qualified head, returns -42; `fitsInIntType(-42,
// sbyte)` is true; the §5.1 narrowing accepts. Verified by returning 0.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           var_sbyte_narrows_from_negative_qualified_const_spec_5_1_17_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public const GK: int := -42; "
        "  public static method Main(): int "
        "    var b: sbyte := C.GK; "
        "    if b = -42 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §17.14 / §5.1 (#2986): a qualified `short` target. GK = 1000 narrows from
// int to short (fitsInIntType(1000, short) is true). Verified by returning 0.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           var_short_narrows_from_qualified_const_spec_5_1_17_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public const GK: int := 1000; "
        "  public static method Main(): int "
        "    var s: short := C.GK; "
        "    if s = 1000 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §17.14 / §5.1 (#2986): the EXPLICITLY-`static` member const — `static const`
// is just a const marked isStatic; the new arm does not require it, but the
// shape `C.GK` where GK is declared `static const` must still fold. Verified
// by returning 0.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           var_byte_narrows_from_explicitly_static_const_spec_5_1_17_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public static const GK: int := 11; "
        "  public static method Main(): int "
        "    var b: byte := C.GK; "
        "    if b = 11 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §17.14 / §5.1 (#2986): the INHERITED-base-class shape. Class `C` extends
// `B`; `B.GX` is a class const on the base; `C.Main()` reads `B.GX` and the
// qualified-name arm walks `lookupMember(B, GX)`, which already followed the
// base-class chain through `lookupMember`'s transitive search (so an inherited
// const is reachable via the SAME arm without a separate case). Verified by
// returning 7.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           var_byte_narrows_from_inherited_qualified_const_spec_5_1_17_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B "
        "  public const GX: int := 7; "
        "end B; "
        "class C : B "
        "  public static method Main(): int "
        "    var b: byte := B.GX; "
        "    if b = 7 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §17.14 / §5.1 (#2986): an INSTANCE method (not Main) reading C.GK through
// `lookupName`'s EnclosingType arm. Before the fix this was REJECTED; the
// fix's qualified-name arm folds C.GK to 42; the §5.1 narrowing accepts.
// Verified end-to-end through `new C().Foo()`.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           instance_method_reads_qualified_const_spec_5_1_17_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public const GK: int := 42; "
        "  public method Foo(): int "
        "    var b: byte := C.GK; "
        "    if b = 42 then return 0; else return 1; end; "
        "  end Foo; "
        "end C; "
        "static class P "
        "  public static method Main(): int return new C().Foo(); end Main; "
        "end P;"),
        0);
}

// §17.14 / §5.1 (#2986): a `const` initializer that names a class const via
// a qualified reference. `B.GX` in a `const` initializer already folded at
// TypeResolution time (#1095); the same shape at expression-position typecheck
// (`var b: byte := B.GX;`) is the new arm. Combined, this proves the
// typecheck-time narrowing agrees with the resolver-time folding on the same
// expression text.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           const_initializer_agrees_with_typecheck_fold_spec_5_1_17_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B "
        "  public const GX: int := 7; "
        "end B; "
        "class C "
        "  public const W: int := B.GX + 1; "
        "  public static method Main(): int "
        "    var b: byte := B.GX; "
        "    if b = 7 then return 0; else return 1; end; "
        "  end Main; "
        "end C;"),
        0);
}

// §17.14 / §22.6 (#2986): a `case` pattern's selector named by a qualified
// const. The pattern-const gate (checkPatternConsts, line 7102-7103 family)
// calls `foldIntConstExprTC` to fold a constant-pattern scrutinee / arm
// selector; a qualified const must fold there too. Returns 0 when the
// matcher picks the qualified arm.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           case_pattern_with_qualified_const_selector_spec_5_1_17_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public const GK: int := 2; "
        "  public static method Main(): int "
        "    var x: int := 2; "
        "    var r := (case x of "
        "      1 => 10, "
        "      C.GK => 0, "
        "      _ => 20); "
        "    return r; "
        "  end Main; "
        "end C;"),
        0);
}

// §15.1 / §17.14 (#2986): an inline-array size using a qualified const. The
// Resolver's `foldIntConstExpr` path already handles this (#1095 sibling
// shape); the typecheck-side folding here proves the two paths agree. A
// `[5]int` array is the resulting type — codegen emits it as a real array,
// confirmed by an indexed store. (Pre-#2986 this could not be parsed as a
// valid type; the const ref folded at type-resolution time but only on the
// resolver's lexical walk. This pin pins that foldIntConstExprTC's qualified
// arm stays compatible — it does NOT widen acceptance past §15.1.)
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           inline_array_size_qualified_const_spec_15_1_17_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  public const GK: int := 5; "
        "  public static method Main(): int "
        "    var a: [C.GK]int; "
        "    a[0] := 11; "
        "    return 42; "
        "  end Main; "
        "end C;"),
        42);
}

// ---------------------------------------------------------------------------
// REJECT — the load-bearing half. The fix LIFTS a rejection, so it must not
// lift any of these. Each passed before the fix and must still pass after it.
// ---------------------------------------------------------------------------

// §5.1: an out-of-range qualified const against a narrower-int target is
// rejected — `byte` cannot absorb 300. The new arm folds C.GK to 300;
// `fitsInIntType(300, byte)` is false; the §5.1 narrowing is a no-op; the
// existing init-path diagnostic still fires.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           out_of_range_qualified_const_still_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public const GK: int := 300; "
        "  public static method Main(): int "
        "    var b: byte := C.GK; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §5.1: an out-of-range qualified const against `sbyte` where the value is
// NEGATIVE-out-of-range for an UNsigned target — `byte` cannot absorb -1.
// `fitsInIntType(-1, byte)` returns false; the §5.1 narrowing stays a no-op.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           negative_qualified_const_to_unsigned_byte_still_rejected_spec_5_1) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public const GK: int := -1; "
        "  public static method Main(): int "
        "    var b: byte := C.GK; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §17.14 / §5.1: a STATIC FIELD (NOT a `const`) referenced as `C.GK` still
// fails the §5.1 narrowing. The new arm `dynamic_cast<ConstMemberSymbol>` is
// specific to a `const` member; a FieldMemberSymbol goes through and returns
// nullopt from the qualified path, falling back to the historical narrowing
// diagnostic. This pins that the fix did not widen acceptance past §17.14.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           static_field_not_const_still_rejected_spec_17_14) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public static GK: int := 42; "
        "  public static method Main(): int "
        "    var b: byte := C.GK; "
        "    return b; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §17.14 / §5.1: an UNRESOLVABLE head — `C` is not in scope — still produces
// the narrowing diagnostic, not a silent fold or accept. The new arm
// `dynamic_cast<TypeSymbol*>` is required; a missing head returns nullopt
// and the historical path takes over.
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           unresolvable_head_still_rejected_spec_17_14) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  public const GK: int := 42; "
        "  public static method Main(): int "
        "    var b: byte := Unknown.K; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §17.14 / §5.1: a runtime `int` variable's value (not a compile-time
// constant) keeps the narrowing diagnostic. The new arm is specific to a
// `ConstMemberSymbol`; an instance field of the enclosing class is a
// FieldSymbol — the arm bypasses, returns nullopt, and §5.1 stays a no-op
// for a runtime value (the historical behavior).
BEZEL_TEST(sema_qualified_const_int_fold_2986,
           instance_field_runtime_value_still_rejected_spec_17_14) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public _v: int; "
        "  public method Main(): int "
        "    var b: byte := _v; "
        "    return b; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
