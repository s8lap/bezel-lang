// Spec-derived regression test for issue #2888.
//
// §7.11 (Lifted operators) / §6.6 (Compound assignments) / §17.6 (The increment
// and decrement operators) / §7.2 (Nullable value types) / §23.3 (Block-form
// properties) / §23.4 (Indexers) / §5.2 (Explicit conversions — the `char`
// code-point check) / §6.4 (Decimal arithmetic), v0.240.
//
// THE DEFECT. A NULLABLE-VALUE-TYPED (`int?`, `long?`, `double?`, `char?`,
// `decimal?`, `byte?`, `uint?`) compound assignment or increment was broken
// EVERYWHERE except one spelling. Measured on `5bf99c0`:
//
//     b.NP  += 3;    ICE — Call parameter type does not match function
//     b[0]  += 4;          signature! … @llvm.sadd.with.overflow.sl_i8i32s
//     iv.DP += 5.0;        ({ i8, i32 } %14, i32 3)
//     a[0]  += 3;    ICE — same, Array<int?>
//     d[1]  += 4;    ICE — same, Dictionary<int,int?>
//     iv.DP++;       ICE — Call parameter type … i37 1
//     n++;           ICE — Call parameter type … i5 1   (a plain LOCAL `int?`!)
//     b[0]++;        rejected with an internal "#1386" error on a legal program
//     b.NP++;        SILENTLY DROPPED — measured 5 where 6 is correct
//
// THE CAUSE — TWO one-definition holes, each with a fan of hand-written copies.
//
//  (1) §7.11's LIFTING existed in exactly ONE place: the open-coded `#1964`
//      block in `lowerStmt`'s `AssignmentStmt` arm. A nullable value type lowers
//      to the tagged `{i8 hasValue; T}` struct (§7.2), and §7.11 defines the
//      operator on it as a CONTROL-FLOW construct — verbatim: "A lifted binary
//      operator produces `null` if *either* operand is `null`", with the runtime
//      checks applying "only when both operands are present". Every OTHER caller
//      of `Emitter::emitCompoundOp` — the block-form property
//      (`lowerPropertyCompound`), the user indexer (`lowerIndexerCompoundCore`),
//      the virtual/interface property (the #481 arm of `lowerStmt`), the
//      imported property (`lowerImportedPropertyCompoundCore`) and the
//      heap-collection element (`lowerCollectionIndexerCompound`) — handed the
//      whole tagged struct to `llvm.sadd.with.overflow` and aborted the compile.
//
//  (2) §17.6's IMPLICIT `1` — verbatim: "For built-in numeric types, `x++` is
//      equivalent to `x := x + 1`" — had SIX hand-written copies, because the
//      desugaring has no AST node for that `1` and each increment lowering
//      synthesized it at the read's own LLVM type. #2883 taught three of them
//      about the struct-typed `decimal`; the other three still called
//      `llvm::ConstantInt::get` on a struct type, which in a no-assert build is
//      an unchecked cast that fabricated a nonsense `i5 1` / `i37 1`. Where the
//      copy DID gate on the lowered type, a nullable slot matched no arm, so
//      `one` came out null — and the two callers differed on what to do about
//      it: one declined with an internal error, one dropped the write silently.
//
// THE FIX puts both rules in ONE place each. `emitCompoundOp` gains the §7.11
// lifting arm, ahead of its `char` arm (which would otherwise range-check the
// whole tagged struct) and its integer tail; the ~110-line open-coded copy in
// `lowerStmt` is DELETED and that site is now the plain call. `emitImplicitOne`
// becomes the single definition of §17.6's implicit right operand, replacing all
// six copies. `isCharTargetExpr` widens to `char?` — the lifting arm unwraps the
// payload before the char arm sees it, so §5.2's code-point check still applies
// on the both-present branch (T8 pins that; without the widening a `char?`
// property compound would silently store an invalid `char`).
//
// Every case asserts a CORRECT VALUE or a CORRECT RAISE, not merely that the
// program compiles. Observables are kept under 256 (the exit status is
// WEXITSTATUS-masked to 8 bits); `decimal` values travel on stdout.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// One class carrying every accessor shape over separate nullable backing
// fields: a block-form `int?` property, a `long?` user indexer, and a
// `double?` property reached through an INTERFACE (itable dispatch).
const char* kBox2888 =
    "public interface IHasD2888 "
    "  property DP: double?; "
    "end IHasD2888; "
    "public class Box2888: IHasD2888 "
    "  public _n: int? := 1; "
    "  public _l: long? := 1; "
    "  public _d: double? := 1.0; "
    "  public property NP: int? "
    "    get => _n; "
    "    set => do _n := value; end; "
    "  end; "
    "  public property DP: double? "
    "    get => _d; "
    "    set => do _d := value; end; "
    "  end; "
    "  public property [i: int]: long? "
    "    get => _l; "
    "    set => do _l := value; end; "
    "  end; "
    "end Box2888; ";

std::string program2888(const std::string& body) {
    return std::string("module M2888; ") + kBox2888 +
           "public class Main2888 "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end Main2888;";
}

int run(const std::string& body) {
    auto r = runAot(program2888(body));
    return r.ran ? r.exitCode : -999;
}

// A body-only program with no Box2888 (for the local/collection cases).
int runPlain(const std::string& body) {
    auto r = runAot(
        std::string("module MP2888; public class C2888 "
                    "  public static method Main(): int ") +
        body + "  end Main; end C2888;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// T1 — the issue's three reported rows, in one program: the block-form `int?`
// property, the `long?` user indexer and the `double?` INTERFACE property, each
// under §6.6's compound desugaring. §6.6, verbatim (the document's hard line
// wrap joined with a single space): "the statement `x op= e` is equivalent to
// `x := x op e` with the target evaluated **once**".
// Pre-fix: the compile ABORTED with three IR-verifier errors. Post-fix: 15 —
// (1+3) + (1+4) + (1.0+5.0).
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, accessor_compound_7_11) {
    BEZEL_EXPECT_EQ(run(
        "    using var b := new Box2888(); "
        "    b.NP += 3; "
        "    b[0] += 4; "
        "    var iv: IHasD2888 := b; "
        "    iv.DP += 5.0; "
        "    return (b.NP ?? -100) + int(b[0] ?? -100L) "
        "           + int(iv.DP ?? -100.0); "), 15);
}

// ===========================================================================
// T2 — §17.6 on the same three accessor shapes: "For built-in numeric types,
// `x++` is equivalent to `x := x + 1` (and likewise `x--` to `x := x - 1`)".
// Pre-fix these failed THREE DIFFERENT WAYS — `b.NP++` dropped the write with
// no diagnostic at all, `b[0]++` was rejected with an internal "#1386" error,
// and `iv.DP++` aborted the compile on a fabricated `i37 1`. Post-fix: 6 —
// (1+1) + (1+1) + (1.0+1.0).
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, accessor_increment_17_6) {
    BEZEL_EXPECT_EQ(run(
        "    using var b := new Box2888(); "
        "    b.NP++; "
        "    b[0]++; "
        "    var iv: IHasD2888 := b; "
        "    iv.DP++; "
        "    return (b.NP ?? -100) + int(b[0] ?? -100L) "
        "           + int(iv.DP ?? -100.0); "), 6);
}

// ===========================================================================
// T3 — §7.11's null rule through every accessor shape and both spellings.
// §7.11, verbatim: "A lifted binary operator produces `null` if *either*
// operand is `null`". A `null` target stays `null` under `+=` AND under `++`.
// (NOT vacuous: T1/T2 prove the same statements produce a VALUE when the target
// is present, so "still null" here is the lifted rule, not a dropped write —
// the pre-fix `b.NP++` also left the field unchanged, but T2 catches that.)
// Post-fix: 63 — all six checks.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, accessor_null_propagates_7_11) {
    BEZEL_EXPECT_EQ(run(
        "    var acc := 0; "
        "    using var b := new Box2888(); "
        "    b._n := null; b._l := null; b._d := null; "
        "    var iv: IHasD2888 := b; "
        "    b.NP += 3; "
        "    b[0] += 4; "
        "    iv.DP += 5.0; "
        "    if b.NP = null then acc := acc + 1; end; "
        "    if b[0] = null then acc := acc + 2; end; "
        "    if iv.DP = null then acc := acc + 4; end; "
        "    b.NP++; "
        "    b[0]++; "
        "    iv.DP++; "
        "    if b.NP = null then acc := acc + 8; end; "
        "    if b[0] = null then acc := acc + 16; end; "
        "    if iv.DP = null then acc := acc + 32; end; "
        "    return acc; "), 63);
}

// ===========================================================================
// T4 — §7.11's null rule from the RIGHT operand: "produces `null` if *either*
// operand is `null`", and "A mixed expression in which one operand is `T?` and
// the other is the non-nullable `T` lifts as well — the non-nullable operand is
// treated as present (never `null`)". A PRESENT target plus a NULL nullable RHS
// yields `null`; a present target plus a PRESENT nullable RHS yields the value.
// Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, nullable_rhs_7_11) {
    BEZEL_EXPECT_EQ(run(
        "    var acc := 0; "
        "    using var b := new Box2888(); "
        "    var some: int? := 41; "
        "    b.NP += some; "
        "    if (b.NP ?? -100) = 42 then acc := acc + 1; end; "
        "    using var c := new Box2888(); "
        "    var none: int? := null; "
        "    c.NP += none; "
        "    if c.NP = null then acc := acc + 2; end; "
        "    using var d := new Box2888(); "
        "    d.NP += 7; "
        "    if (d.NP ?? -100) = 8 then acc := acc + 4; end; "
        "    return acc; "), 7);
}

// ===========================================================================
// T5 — the LOCAL `n++` on a nullable value. The issue placed the whole defect
// on the accessor paths because `n += 1` on a local was already lifted; the
// `++` SPELLING of the same statement was not, and aborted the compile on a
// fabricated `i5 1` constant. §17.6 identifies the two, so they must agree.
// Post-fix: 21 — 6 (`n++`) + 4 (`n--`) + 11 (`n += 1` control on the same
// local, which passes both ways and pins that the two spellings agree).
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, local_increment_17_6) {
    BEZEL_EXPECT_EQ(runPlain(
        "    var a: int? := 5; a++; "
        "    var b: int? := 5; b--; "
        "    var c: int? := 10; c += 1; "
        "    return (a ?? -100) + (b ?? -100) + (c ?? -100); "), 21);
}

// ===========================================================================
// T6 — the LOCAL `n++` on a NULL nullable value stays `null` (§7.11 read
// through §17.6's `x := x + 1` identity: the implicit `1` is a present
// non-nullable operand, so the result is null only by reason of the target).
// Post-fix: 3.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, local_increment_null_7_11) {
    BEZEL_EXPECT_EQ(runPlain(
        "    var acc := 0; "
        "    var a: int? := null; a++; "
        "    if a = null then acc := acc + 1; end; "
        "    var d: double? := null; d--; "
        "    if d = null then acc := acc + 2; end; "
        "    return acc; "), 3);
}

// ===========================================================================
// T7 — the HEAP-COLLECTION element spellings, which the issue did not list:
// `Array<int?>` and `Dictionary<int,int?>` reach emitCompoundOp through
// `lowerCollectionIndexerCompound` / `lowerCollectionIndexerIncrDecr` and were
// the same ICE. stdlib §4.4/§4.8. Post-fix: 39 — 14 (array 10+3, then ++) +
// 25 (dict 20+4, then ++).
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, collection_element_6_6) {
    BEZEL_EXPECT_EQ(runPlain(
        "    using var a := new Array<int?>(); "
        "    a.Add(10); "
        "    a[0] += 3; "
        "    a[0]++; "
        "    using var d := new Dictionary<int, int?>(); "
        "    d.Add(1, 20); "
        "    d[1] += 4; "
        "    d[1]++; "
        "    return (a[0] ?? -100) + (d[1] ?? -100); "), 39);
}

// ===========================================================================
// T8 — §5.2's `char` code-point write-back survives the lift, on EVERY branch.
// §6.6 desugars a `char` target to `c := char(c OP e)`, and §5.2 raises
// OverflowException outside U+0000–U+10FFFF. §7.11: that check "appl[ies] only
// when both operands are present". This is the case that pins the
// `isCharTargetExpr` widening — with `char?` excluded the present branch would
// store an out-of-range `char` with no raise at all. Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, char_nullable_property_5_2) {
    const char* boxC =
        "public class CBox2888 "
        "  public _c: char? := char('A'); "
        "  public property CP: char? "
        "    get => _c; "
        "    set => do _c := value; end; "
        "  end; "
        "end CBox2888; ";
    auto r = runAot(
        std::string("module MC2888; ") + boxC +
        "public class MainC2888 "
        "  public static method Main(): int "
        "    var acc := 0; "
        // present: 'A' + 1 = 'B' (66)
        "    using var b := new CBox2888(); "
        "    b.CP += 1; "
        "    if int(b.CP ?? char(0)) = 66 then acc := acc + 1; end; "
        // present and out of range: §5.2 raises
        "    using var b2 := new CBox2888(); "
        "    b2._c := char(0x10FFFF); "
        "    try b2.CP += 1; catch OverflowException => acc := acc + 2; end; "
        // null at the same code point: §7.11 — no underlying op, no check
        "    using var b3 := new CBox2888(); "
        "    b3._c := null; "
        "    try b3.CP += 1; acc := acc + 4; "
        "    catch OverflowException => acc := acc + 0; end; "
        "    return acc; "
        "  end Main; "
        "end MainC2888;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 7);
}

// ===========================================================================
// T9 — `decimal?` through a property: the payload is the `%bz.decimal` struct
// rather than a scalar, so the lift routes it to the §6.4 intrinsic. §6.4:
// decimal arithmetic "raises `OverflowException` on overflow and
// `DivideByZeroException` on division by zero"; §7.11 names decimal
// DivideByZeroException explicitly as a present-branch-only check.
// Post-fix: 7 (value 9 = 5 + 3 + 1 checked separately on stdout).
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, decimal_nullable_property_6_4) {
    const char* boxM =
        "public class MBox2888 "
        "  public _m: decimal? := 5m; "
        "  public property MP: decimal? "
        "    get => _m; "
        "    set => do _m := value; end; "
        "  end; "
        "end MBox2888; ";
    auto head = std::string("module MM2888; ") + boxM +
                "public class MainM2888 "
                "  public static method Main(): int ";
    // The lifted value itself, on stdout (a decimal does not fit an exit code).
    BEZEL_EXPECT_EQ(runAotOut(
        head +
        "    using var b := new MBox2888(); "
        "    b.MP += 3m; "
        "    b.MP++; "
        "    Console.WriteLine((b.MP ?? 0m).ToString()); "
        "    return 0; "
        "  end Main; "
        "end MainM2888;"), "9");
    // The §6.4 raise, present branch only.
    auto r = runAot(
        head +
        "    var acc := 0; "
        "    using var b := new MBox2888(); "
        "    try b.MP /= 0m; catch DivideByZeroException => acc := acc + 1; end; "
        "    using var c := new MBox2888(); "
        "    c._m := null; "
        "    try c.MP /= 0m; acc := acc + 2; "
        "    catch DivideByZeroException => acc := acc + 0; end; "
        "    if c.MP = null then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end MainM2888;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 7);
}

// ===========================================================================
// T10 — §6.6's CHECKED narrow-integer write-back and §6.2's signedness survive
// the lift through an accessor. §6.6, verbatim (hard line wraps joined with a
// single space): "`b += 10` on `b: byte` is `b := byte(b + 10)` — the addition
// runs in `int` per §6.2, and a result outside `byte`'s range raises
// `OverflowException`". And an UNSIGNED payload
// must select unsigned arithmetic, or `uint? += 1` near the top of the range
// would report a false overflow. Post-fix: 7.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, checked_writeback_and_signedness_6_6) {
    const char* boxN =
        "public class NBox2888 "
        "  public _b: byte? := null; "
        "  public _u: uint? := 4000000000U; "
        "  public property BP: byte? "
        "    get => _b; "
        "    set => do _b := value; end; "
        "  end; "
        "  public property UP: uint? "
        "    get => _u; "
        "    set => do _u := value; end; "
        "  end; "
        "end NBox2888; ";
    auto r = runAot(
        std::string("module MN2888; ") + boxN +
        "public class MainN2888 "
        "  public static method Main(): int "
        "    var acc := 0; "
        // 200 + 100 = 300, out of byte range -> OverflowException
        "    using var b := new NBox2888(); "
        "    b._b := 200; "
        "    try b.BP += 100; catch OverflowException => acc := acc + 1; end; "
        // null byte?: no underlying op, no check (§7.11)
        "    using var c := new NBox2888(); "
        "    try c.BP += 100; acc := acc + 2; "
        "    catch OverflowException => acc := acc + 0; end; "
        // unsigned: 4000000000 + 1 must NOT be read as a signed overflow
        "    using var d := new NBox2888(); "
        "    d.UP += 1; "
        "    if (d.UP ?? 0U) = 4000000001U then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end MainN2888;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 7);
}

// ===========================================================================
// T11 — §7.4 NARROWED spelling. §7.4 narrowing deliberately does not rewrite
// the target's `exprType`, so the compound inside a narrowed branch still sees
// a `T?` target and must take the lifted path (and still produce the value).
// Post-fix: 42.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_target_7_4) {
    BEZEL_EXPECT_EQ(run(
        "    using var b := new Box2888(); "
        "    b._n := 40; "
        "    if b.NP <> null then "
        "      b.NP += 1; "
        "      b.NP++; "
        "    end; "
        "    return (b.NP ?? -100); "), 42);
}

// ===========================================================================
// T12 — ACCEPT CONTROL (deliberate over-reach probe). The NON-nullable
// spellings of every shape this fix touches must be bit-identical: the lifting
// arm is keyed on the tagged-struct LOWERING, so a plain `int`/`double`/`char`/
// `decimal`/`string` property, indexer, interface property or collection
// element must not enter it, and §17.6's implicit `1` must still be produced
// for each. Passes both with and without the fix BY CONSTRUCTION — its job is
// to fail if the change over-reaches. Post-fix: 255 — all eight shapes.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, non_nullable_unchanged_control) {
    const char* boxP =
        "public interface IHasQ2888 "
        "  property QD: double; "
        "end IHasQ2888; "
        "public class PBox2888: IHasQ2888 "
        "  public _i: int := 0; "
        "  public _d: double := 0.0; "
        "  public _c: char := char('A'); "
        "  public _s: string := \"a\"; "
        "  public property IP: int "
        "    get => _i; "
        "    set => do _i := value; end; "
        "  end; "
        "  public property QD: double "
        "    get => _d; "
        "    set => do _d := value; end; "
        "  end; "
        "  public property CP: char "
        "    get => _c; "
        "    set => do _c := value; end; "
        "  end; "
        "  public property SP: string "
        "    get => _s; "
        "    set => do _s := value; end; "
        "  end; "
        "  public property [i: int]: int "
        "    get => _i; "
        "    set => do _i := value; end; "
        "  end; "
        "end PBox2888; ";
    auto r = runAot(
        std::string("module MPB2888; ") + boxP +
        "public class MainP2888 "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new PBox2888(); "
        "    b.IP += 3; b.IP++; "                       // 4
        "    if b._i = 4 then acc := acc + 1; end; "
        "    using var c := new PBox2888(); "
        "    c[0] += 5; c[0]++; "                       // 6
        "    if c._i = 6 then acc := acc + 2; end; "
        "    using var d := new PBox2888(); "
        "    var iq: IHasQ2888 := d; "
        "    iq.QD += 7.0; iq.QD++; "                   // 8.0
        "    if int(d._d) = 8 then acc := acc + 4; end; "
        "    using var e := new PBox2888(); "
        "    e.CP += 1; e.CP++; "                       // 'C'
        "    if int(e._c) = 67 then acc := acc + 8; end; "
        "    using var f := new PBox2888(); "
        "    f.SP += \"bc\"; "                          // "abc"
        "    if f._s = \"abc\" then acc := acc + 16; end; "
        "    using var g := new Array<int>(); "
        "    g.Add(10); g[0] += 3; g[0]++; "            // 14
        "    if g[0] = 14 then acc := acc + 32; end; "
        "    using var h := new Dictionary<int, int>(); "
        "    h.Add(1, 20); h[1] += 4; h[1]++; "         // 25
        "    if h[1] = 25 then acc := acc + 64; end; "
        "    var loc: int := 5; loc++; loc += 2; "      // 8
        "    if loc = 8 then acc := acc + 128; end; "
        "    return acc; "
        "  end Main; "
        "end MainP2888;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 255);
}
