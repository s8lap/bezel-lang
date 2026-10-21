// Spec-derived regression test for issue #2925.
//
// §5.1 (Implicit conversions — "Constant narrowing" and "Nullable
// conversions") / §17.14 (Constant expressions) / §22.4 (Arguments and
// parameters) / §22.5 (Default values) / §9.1 (Record positional fields) /
// §12.1 (Virtual dispatch), v0.240.
//
// THE RULE, verbatim (v0.240 §5.1):
//
//   "**Constant narrowing.** An expression of integer type that is a
//    **compile-time constant** ([§17.14]) implicitly converts to any integer
//    type whose range contains its value: `var b: byte := 50;`, `Set(level:
//    byte)` called as `Set(3)`, and enum constants of the underlying type
//    ([§10.2]) are all well-typed by this rule. A constant whose value does not
//    fit the target's range is a **compile error** at the conversion site —
//    never a silent truncation. The rule applies to constants only; a run-time
//    `int` value still requires the explicit `byte(x)` narrowing of [§5.2].
//    Collection-literal element typing ([§15.4]) and default-argument typing
//    use this same rule."
//
// and, under "Nullable conversions": "The two rules compose through at most one
// `?` on each side ([§7.10]: `?` is idempotent): `S` converts to `T?` via
// `S → T → T?`".
//
// THE DEFECT. The predicate was right; the POPULATION it was fed never
// contained most of §5.1's conversion sites. The re-type existed in exactly
// three hand-written places — a LOCAL declaration, an ASSIGNMENT statement, and
// a RECORD POSITIONAL `new Rec(...)` argument (which carried its own third copy
// of the range table) — so everywhere else the constant kept its `int` type.
// MEASURED on 618c0c8:
//
//   new Cfg(lvl := 3) / c.Set(3) / Cfg.SSet(3)
//        "the argument `level` … has a type that is not implicitly convertible
//         to the target type (§5.1)" — a LOUD FALSE REJECT of the example
//        §5.1 itself gives for the rule.
//   return 200;  from `byte` and from `byte?`
//        "the returned value … is not implicitly convertible …"
//   method W(level: byte := 3)
//        ADMITTED by sema, then MALFORMED IR: "Call parameter type does not
//        match function signature! i32 3 / i8" — a default-value expression was
//        never checked against its parameter type at all.
//
// THE FIX gives the rule ONE definition — `ExprTC::narrowIntConstantToTarget`,
// called from `checkAssignable` itself, which IS the §5.1 conversion-site check
// — and deletes the three copies, so the population is "every site that asks
// the §5.1 question". The default-value expression, which asked no question,
// now runs the same `checkAssignable`.
//
// The CODEGEN half is the other direction of the same one-definition point.
// `coerceNumericToType` is §5.1's WIDENING direction only by construction
// (`srcBits < targetBits`), exactly as #2903 found at the field-initializer
// stores and #2927 at the property setter slot. A LITERAL lowers at its
// re-typed width (lowerExpr reads exprType), but `const K := 3` loads its i32
// slot and `1 + 2` folds at i32 — so `Take(K)` into a `byte` parameter emitted
// `call i32 @Take(i32 %k)` and `return K;` from a `byte` method emitted
// `ret i32` from an i8 function. The call-argument, return, arrow-body and
// nullable-return-payload sites now use #2903's `coerceInitializerToSlot`,
// which reaches the slot at the SLOT's width in BOTH of §5.1's directions.
//
// Every case asserts a VALUE, never merely that the program compiles.
// Observables stay under 256 — the exit status is WEXITSTATUS-masked to 8 bits.
// The out-of-range and non-constant REJECT controls (which must keep failing —
// "never a silent truncation") are the lit sibling,
// tests/lit/constant_narrowing_conversion_sites_reject_2925.bzl.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// T1 — §5.1's OWN example: "`Set(level: byte)` called as `Set(3)`". A class
// constructor argument, an instance method argument and a static method
// argument, all with a plain int literal.
// Pre-fix: three "not implicitly convertible" errors → the program does not
// compile at all (-999). Post-fix: 15.
// ===========================================================================
BEZEL_TEST(codegen_constant_narrowing_sites_2925, spec_set_3_example_5_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2925a; "
        "public class Cfg "
        "  public _lvl: byte := 0; "
        "  public constructor(lvl: byte) "
        "    _lvl := lvl; "
        "  end; "
        "  public method Set(level: byte): int "
        "    _lvl := level; "
        "    return int(level); "
        "  end Set; "
        "  public static method SSet(level: byte): int "
        "    return int(level); "
        "  end SSet; "
        "end Cfg; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var c := new Cfg(lvl := 3); "
        "    if int(c._lvl) = 3 then acc := acc + 1; end; "
        "    if c.Set(5) = 5 then acc := acc + 2; end; "
        "    if int(c._lvl) = 5 then acc := acc + 4; end; "
        "    if Cfg.SSet(7) = 7 then acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 15);
}

// ===========================================================================
// T2 — `return`, in all four of its spellings (block `return`, arrow body) and
// both of §5.1's nullability compositions (`S → T` and `S → T → T?`). The
// const-identifier and folded-arithmetic rows additionally pin the CODEGEN
// half: those lower at i32 even after the re-type, so `ret i32` from an i8/i16
// function is what the widening-only coercion left behind.
// Pre-fix: the literal rows are sema false rejects and the rest never compile
// → -999. Post-fix: 63.
// ===========================================================================
BEZEL_TEST(codegen_constant_narrowing_sites_2925, return_value_5_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2925b; "
        "public class M "
        "  public static method RetLit(): byte "
        "    return 200; "
        "  end RetLit; "
        "  public static method RetConst(): byte "
        "    const K := 42; "
        "    return K; "
        "  end RetConst; "
        "  public static method RetFold(): sbyte "
        "    return 40 + 2; "
        "  end RetFold; "
        "  public static method RetArrow(): byte => 77; "
        "  public static method RetQ(): byte? "
        "    const K := 200; "
        "    return K; "
        "  end RetQ; "
        "  public static method RetSQ(): short? "
        "    return 20000 + 1; "
        "  end RetSQ; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    if int(M.RetLit())   = 200   then acc := acc + 1;  end; "
        "    if int(M.RetConst()) = 42    then acc := acc + 2;  end; "
        "    if int(M.RetFold())  = 42    then acc := acc + 4;  end; "
        "    if int(M.RetArrow()) = 77    then acc := acc + 8;  end; "
        "    if (M.RetQ()  ?? 0) = 200   then acc := acc + 16; end; "
        "    if (M.RetSQ() ?? -1) = 20001 then acc := acc + 32; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 63);
}

// ===========================================================================
// T3 — a DEFAULT ARGUMENT, which §5.1 names explicitly ("default-argument
// typing use this same rule"). This site ran NO assignability check at all, so
// sema admitted the mismatch and codegen emitted a malformed `call`.
// Pre-fix: "Call parameter type does not match function signature! i32 3 / i8"
// → IR verification failure → -999. Post-fix: 31.
// ===========================================================================
BEZEL_TEST(codegen_constant_narrowing_sites_2925, default_argument_22_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2925c; "
        "public class M "
        "  public static method D1(level: byte := 3): int "
        "    return int(level); "
        "  end D1; "
        "  public static method D2(level: short := 20000 + 1): int "
        "    return int(level); "
        "  end D2; "
        "  public static method D3(level: byte? := 9): int "
        "    if level is null then return -1; end; "
        "    return int(level); "
        "  end D3; "
        "  public static method D4(level: sbyte := -100): int "
        "    return int(level); "
        "  end D4; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    if M.D1()  = 3     then acc := acc + 1;  end; "
        "    if M.D1(7) = 7     then acc := acc + 2;  end; "
        "    if M.D2()  = 20001 then acc := acc + 4;  end; "
        "    if M.D3()  = 9     then acc := acc + 8;  end; "
        "    if M.D4()  = -100  then acc := acc + 16; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 31);
}

// ===========================================================================
// T4 — every integer target the rule covers (§5.1's table: the six types an
// `int` does not already widen to), at a call argument, in all three constant
// spellings. The const-identifier row is the one the CODEGEN half is about:
// a `const` local loads its i32 slot regardless of the re-typed sema type.
// Pre-fix: six "not implicitly convertible" errors → -999. Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_constant_narrowing_sites_2925, every_narrower_width_argument_5_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2925d; "
        "public class M "
        "  public static method Take(a: sbyte, b: byte, c: short, d: ushort, "
        "                            e: uint, f: ulong): long "
        "    return long(a) + long(b) + long(c) + long(d) + long(e) + long(f); "
        "  end Take; "
        "  public static method Main(): int "
        "    const A := -100; "
        "    const B := 200; "
        "    const C := -30000; "
        "    const D := 60000; "
        "    const E := 2000000000; "
        "    const F := 9; "
        "    var acc := 0; "
        "    if M.Take(A, B, C, D, E, F) = 2000030109L "
        "      then acc := acc + 1; end; "
        "    if M.Take(-100, 200, -30000, 60000, 2000000000, 9) = 2000030109L "
        "      then acc := acc + 2; end; "
        "    if M.Take(-50 - 50, 100 + 100, -30000, 60000, 2000000000, 3 * 3) "
        "        = 2000030109L then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 7);
}

// ===========================================================================
// T5 — the DISPATCHED argument paths (§12.1 virtual, §11.2 interface), which
// go through a different codegen arg-coercion loop from the direct call.
// Pre-fix: three "not implicitly convertible" errors → -999. Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_constant_narrowing_sites_2925, virtual_and_interface_argument_12_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2925e; "
        "public interface IFace "
        "  method Take(b: byte): int; "
        "end IFace; "
        "public class Base "
        "  public virtual method V(b: byte): int "
        "    return int(b) + 1000; "
        "  end V; "
        "end Base; "
        "public class Derived: Base, IFace "
        "  public override method V(b: byte): int "
        "    return int(b) + 2000; "
        "  end V; "
        "  public method Take(b: byte): int "
        "    return int(b) + 3000; "
        "  end Take; "
        "end Derived; "
        "public class M "
        "  public static method Main(): int "
        "    const K := 5; "
        "    var acc := 0; "
        "    using var d := new Derived(); "
        "    if d.V(K) = 2005 then acc := acc + 1; end; "
        "    var b: Base := d; "
        "    if b.V(1 + 1) = 2002 then acc := acc + 2; end; "
        "    var i: IFace := d; "
        "    if i.Take(K) = 3005 then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 7);
}

// ===========================================================================
// T6 — ACCEPT-CONTROL for the copy the fix DELETED. The record positional path
// (§9.1) carried its own pre-pass that re-typed each argument before
// `checkArgsAgainstParams`; that pre-pass is gone and the shared rule inside
// `checkAssignable` must cover it, including a reordered NAMED argument list
// (which is why the deleted copy bound the arguments first).
// Passes both pre- and post-fix BY CONSTRUCTION — its job is to prove the
// deletion changed nothing.
// ===========================================================================
BEZEL_TEST(codegen_constant_narrowing_sites_2925, record_positional_accept_control_9_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2925f; "
        "public record Pt(x: byte, y: short?); "
        "public class M "
        "  public static method Main(): int "
        "    const K := 5; "
        "    var acc := 0; "
        "    var p := new Pt(K, 30000); "
        "    if int(p.x) = 5 then acc := acc + 1; end; "
        "    if (p.y ?? -1) = 30000 then acc := acc + 2; end; "
        "    var q := new Pt(y := 30000, x := 200); "
        "    if int(q.x) = 200 then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 7);
}

// ===========================================================================
// T8 — the BUILTIN collection / atomic argument gates, which #2652 recorded as
// rejecting the same constant with their own per-callee wording ("argument to
// `Array<T>.IndexOf` … (§5.1)"). They reach the same `checkAssignable`, so
// hoisting the rule into it covers them too — and each stores the RIGHT value,
// not a truncated one, which is what distinguishes a fixed conversion from a
// suppressed diagnostic.
// Pre-fix: four "not implicitly convertible" errors → -999. Post-fix: 109.
// ===========================================================================
BEZEL_TEST(codegen_constant_narrowing_sites_2925, builtin_collection_argument_2652) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2925h; "
        "public class M "
        "  public static method Main(): int "
        "    using var a := new Array<byte>(4, 0); "
        "    using var s := new Set<byte>(); "
        "    using var d := new Dictionary<string, byte>(); "
        "    using var at := new Atomic<byte>(0); "
        "    var acc := 0; "
        "    if a.IndexOf(3) = -1 then acc := acc + 1; end; "
        "    s.Add(3); "
        "    d.Add(\"a\", 3); "
        "    if int(at.Exchange(3)) = 0 then acc := acc + 4; end; "
        "    if int(at.Value) = 3 then acc := acc + 8; end; "
        "    if s.Contains(3) then acc := acc + 32; end; "
        "    if int(d[\"a\"]) = 3 then acc := acc + 64; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 109);
}

// ===========================================================================
// T7 — ACCEPT-CONTROL for the LOCAL-declaration and ASSIGNMENT sites, whose
// hand-written copies the fix also deleted in favour of the shared rule. These
// are the two shapes #738 originally fixed; they must be byte-for-byte
// unchanged. Passes both pre- and post-fix BY CONSTRUCTION.
// ===========================================================================
BEZEL_TEST(codegen_constant_narrowing_sites_2925, local_and_assignment_accept_control_738) {
    BEZEL_EXPECT_EQ(runProg(
        "module N2925g; "
        "public class M "
        "  public static method Main(): int "
        "    const K := 3; "
        "    var acc := 0; "
        "    var b: byte := K; "
        "    var c: byte := 1 + 2; "
        "    var d: byte := 5; "
        "    var q: byte? := 200; "
        "    b := K; "
        "    c := 1 + 2; "
        "    d := 250; "
        "    if int(b) = 3 then acc := acc + 1; end; "
        "    if int(c) = 3 then acc := acc + 2; end; "
        "    if int(d) = 250 then acc := acc + 4; end; "
        "    if (q ?? 0) = 200 then acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 15);
}
