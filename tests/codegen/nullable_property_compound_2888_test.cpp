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

// A body-only program with no Box2888 (for the local/collection cases).
int runPlain(const std::string& body) {
    auto r = runAot(
        std::string("module MP2888; public class C2888 "
                    "  public static method Main(): int ") +
        body + "  end Main; end C2888;");
    return r.ran ? r.exitCode : -999;
}

// fix #3830: 1 if `body` raises OverflowException, 0 if it runs to
// completion without it, -999 on a front-end failure. Mirrors codegen_1964's
// `throwsOverflow` (nullable_value_compound_1964_test.cpp) — §6.6's checked
// narrow-integer write-back inherited by a §7.4-narrowed compound target.
int throwsOverflowPlain(const std::string& body) {
    auto r = runAot(
        std::string("module MP2888; public class C2888 "
                    "  public static method Main(): int "
                    "    try ") +
        body +
        " return 0; catch OverflowException => return 1; end; "
        "  end Main; end C2888;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// §7.11 / §6.6 / §17.6 AT v0.286 (#3298) — WHY EVERY NULLABLE ROW BELOW IS NOW
// A REJECTION, AND WHAT #2888's COVERAGE BECAME.
//
// #2888 landed at v0.240 against a §7.11 that LIFTED: "a lifted binary
// operator produces `null` if either operand is `null`". Proposal 0114
// graduated at v0.267 and DELETED that rule. §7.11 (v0.286) now opens "No
// operator **lifts** to a nullable type", §6.6 makes a read-modify-write
// compound "a **compile error** when the static type of its left-hand side, or
// of its right operand, is a **nullable** type — either position, or both, a
// nullable **reference**-typed target included", and §17.6 says the same of
// `++`/`--`. Every row in T1–T11 asserted a VALUE produced by the deleted
// rule, so each is inverted here to assert the diagnosis that replaced it.
//
// This is not a bulk repair: #2888's actual subject was TWO one-definition
// holes — (1) the §7.11 lifting arm that had one copy and five callers that
// lacked it, and (2) §17.6's implicit `1`, which had six hand-written copies.
// Hole (1) no longer exists as a rule, so its coverage is not lost but
// RETIRED. Hole (2) is about the NON-nullable path too, and every one of its
// six spellings — block-form property, user indexer, interface property,
// imported property, heap-collection element, plain local — is still exercised
// with its value by T12 below, which is why T12 is untouched and why it is now
// this file's load-bearing row rather than its control.
//
// §7.4 does NOT rescue a property or an indexer slot: neither is narrowable
// (§7.4 admits a local, a parameter and a `readonly` field only), so every
// accessor row in this file (T1-T4, T7-T9, T11) is unaffected by anything
// below and stays a rejection. A nullable-typed LOCAL target is a different
// story: issue 3686 asked whether §6.6/§17.6 should enumerate by the
// DECLARED type (rejecting even a §7.4-narrowed local) or by the type AFTER
// narrowing, and it has since been RULED at v0.288 (fix #3830): narrowing
// reaches the compound/`++`/`--` forms exactly as it already reached the
// binary form. T5 and T10 are the two rows that ruling flips — see their own
// comments; every other row in this file names a property/indexer target and
// is untouched.
// ===========================================================================

namespace {

// Rejected, and by the §6.6/§17.6 family rather than by any compile error:
// "did not run" alone would pass on an unrelated failure in these long
// programs. Every message this file expects names §7.11's remedies.
bool rejected2888(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        const bool compound =
            m.find("compound assignment") != std::string::npos &&
            m.find("nullable") != std::string::npos;
        const bool increment =
            m.find("is not defined on a nullable") != std::string::npos;
        if (!compound && !increment) continue;
        if (m.find("7.11") == std::string::npos) continue;
        return true;
    }
    return false;
}
bool rejectedBody(const std::string& body) {
    return rejected2888(program2888(body));
}
bool rejectedPlain(const std::string& body) {
    return rejected2888(
        std::string("module MP2888; public class C2888 "
                    "  public static method Main(): int ") +
        body + "  end Main; end C2888;");
}

}  // namespace

// ===========================================================================
// T1 — the issue's three reported rows (block-form `int?` property, user
// `long?` indexer, interface-dispatched `double?` property). Each was an ICE
// at v0.240 and a lifted VALUE from the #2888 fix until v0.267; each is a §6.6
// compile error now. The three shapes are kept in ONE program because what is
// being pinned is that no accessor shape escapes the gate.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, accessor_compound_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedBody(
        "    using var b := new Box2888(); "
        "    b.NP += 3; "
        "    return (b.NP ?? -100); "));
    BEZEL_EXPECT_TRUE(rejectedBody(
        "    using var b := new Box2888(); "
        "    b[0] += 4; "
        "    return int(b[0] ?? -100L); "));
    BEZEL_EXPECT_TRUE(rejectedBody(
        "    using var b := new Box2888(); "
        "    var iv: IHasD2888 := b; "
        "    iv.DP += 5.0; "
        "    return int(iv.DP ?? -100.0); "));
}

// ===========================================================================
// T2 — §17.6 on the same three accessor shapes. §17.6 (v0.286): "An operand
// whose static type is a nullable type is a compile error … this form and
// §6.6's compound assignments are one rule read through one desugaring."
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, accessor_increment_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedBody(
        "    using var b := new Box2888(); b.NP++; return 0; "));
    BEZEL_EXPECT_TRUE(rejectedBody(
        "    using var b := new Box2888(); b[0]++; return 0; "));
    BEZEL_EXPECT_TRUE(rejectedBody(
        "    using var b := new Box2888(); "
        "    var iv: IHasD2888 := b; iv.DP++; return 0; "));
}

// ===========================================================================
// T3 — the absent-target case. It used to assert "null stays null"; §7.11 now
// says the absent operand "is diagnosed at COMPILE time", so the program never
// runs and there is no runtime answer to check. The rejection does not depend
// on what the target holds — which is the point: the gate reads the static
// type, so a `null` target and a present one are rejected alike.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, accessor_null_target_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedBody(
        "    using var b := new Box2888(); "
        "    b._n := null; "
        "    b.NP += 3; "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(rejectedBody(
        "    using var b := new Box2888(); "
        "    b._n := null; "
        "    b.NP++; "
        "    return 0; "));
}

// ===========================================================================
// T4 — §6.6's RIGHT-operand half: "the static type of its left-hand side, OR
// of its right operand". A non-nullable target with a nullable RHS is the same
// compile error, which is what makes the rule symmetric rather than a
// target-only check.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, nullable_rhs_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedPlain(
        "    var n: int? := null; "
        "    var t: int := 5; "
        "    t += n; "
        "    return t; "));
    // fix #3830 (v0.288): `n` is now a PARAMETER, not a literal-initialized
    // local — `var n: int? := 3;` is §7.4-narrowed at its initializer, and
    // narrowing now reaches compound assignment, so `t += n` on a narrowed
    // `n` is admitted (verified empirically: `t` becomes 8) — the exact
    // int_plus_intq shape from tests/lit/sema/compound_nullable_operand_2153.
    // A parameter is never narrowed by what the caller passes, so this keeps
    // testing a genuinely nullable, unguarded RHS.
    BEZEL_EXPECT_TRUE(rejected2888(
        "module MP2888N; class C2888N "
        "  public static method Chk(n: int?): int "
        "    var t: int := 5; "
        "    t += n; "
        "    return t; "
        "  end Chk; "
        "  public static method Main(): int return Chk(3); end Main; "
        "end C2888N;"));
}

// ===========================================================================
// T5 — the plain LOCAL. §17.6's desugaring is §6.6's, so `a++` on an `int?`
// takes the same verdict as `a += 1`. ⚠ THIS ROW FLIPPED at v0.288 (fix
// #3830, ruling on issue 3686 — the "if 3686 rules the other way" case this
// block used to name): `a`/`b`/`c` here ARE §7.4-narrowed by their non-null
// literal initializer, and #3830 makes §6.6/§17.6 read the operand's type
// AFTER narrowing, exactly as §7.11's binary-operator gate already did — so
// the compound/increment forms are now admitted wherever the desugared
// `a := a + 1` spelling is, and produce the SAME value. Un-narrowed twins
// (a `null` initializer) stay rejected in T6 below — the asymmetry this
// block used to document is gone; what remains is the ordinary
// narrowed-vs-not distinction §7.4 draws everywhere else.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, local_increment_narrowed_accepted_3830) {
    BEZEL_EXPECT_EQ(runPlain(
        "    var a: int? := 5; a++; return (a ?? -100); "), 6);
    BEZEL_EXPECT_EQ(runPlain(
        "    var b: int? := 5; b--; return (b ?? -100); "), 4);
    BEZEL_EXPECT_EQ(runPlain(
        "    var c: int? := 10; c += 1; return (c ?? -100); "), 11);
    // The desugared spelling on the same narrowed local, for comparison —
    // #3830 makes the two spellings agree, which is the point.
    BEZEL_EXPECT_EQ(runPlain(
        "    var a: int? := 5; a := a + 1; return (a ?? -100); "), 6);
}

// ===========================================================================
// T6 — the same two spellings on an ABSENT local. Un-narrowed (a `null`
// initialiser establishes nothing), so both the compound form and the
// desugared form are rejected — the desugared escape hatch in T5 exists only
// because §7.4 proved the operand present.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, local_increment_null_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedPlain("    var a: int? := null; a++; return 0; "));
    BEZEL_EXPECT_TRUE(rejectedPlain("    var d: double? := null; d--; return 0; "));
    BEZEL_EXPECT_TRUE(rejectedPlain(
        "    var a: int? := null; a := a + 1; return 0; "));
}

// ===========================================================================
// T7 — the heap-collection element slots (`Array<int?>`, `Dictionary<K,int?>`).
// An element slot is not narrowable storage under §7.4 at all, so the verdict
// here does not depend on issue 3686's reading either way.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, collection_element_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedPlain(
        "    using var a := new Array<int?>(); "
        "    a.Add(10); a[0] += 3; return 0; "));
    BEZEL_EXPECT_TRUE(rejectedPlain(
        "    using var a := new Array<int?>(); "
        "    a.Add(10); a[0]++; return 0; "));
    BEZEL_EXPECT_TRUE(rejectedPlain(
        "    using var d := new Dictionary<int, int?>(); "
        "    d.Add(1, 20); d[1] += 4; return 0; "));
}

// ===========================================================================
// T8 — `char?`. §5.2's code-point write-back was the subject; it is unreachable
// through a nullable target now, and T12 keeps the NON-nullable `char`
// write-back (both the in-range value and the OverflowException) covered.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, char_nullable_property_rejected_3298) {
    const char* boxC =
        "public class CBox2888 "
        "  public _c: char? := char('A'); "
        "  public property CP: char? "
        "    get => _c; "
        "    set => do _c := value; end; "
        "  end; "
        "end CBox2888; ";
    BEZEL_EXPECT_TRUE(rejected2888(
        std::string("module MC2888; ") + boxC +
        "public class MainC2888 "
        "  public static method Main(): int "
        "    using var b := new CBox2888(); "
        "    b.CP += 1; "
        "    return 0; "
        "  end Main; "
        "end MainC2888;"));
}

// ===========================================================================
// T9 — `decimal?` through a property. The §6.4 runtime route for a decimal
// operand is NOT lost with this row: `codegen_lifted_ops_486_narrowed_3298`
// (tests/codegen/lifted_operators_486_test.cpp) exercises it on a §7.4-narrowed
// `decimal?` and checks the produced value, and T12 keeps the non-nullable
// property route.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, decimal_nullable_property_rejected_3298) {
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
    BEZEL_EXPECT_TRUE(rejected2888(
        head +
        "    using var b := new MBox2888(); "
        "    b.MP += 3m; "
        "    return 0; "
        "  end Main; "
        "end MainM2888;"));
    BEZEL_EXPECT_TRUE(rejected2888(
        head +
        "    using var b := new MBox2888(); "
        "    b.MP++; "
        "    return 0; "
        "  end Main; "
        "end MainM2888;"));
}

// ===========================================================================
// T10 — §6.6's checked narrow-integer write-back and §6.2's signedness. ⚠
// FLIPPED at v0.288 (fix #3830): `y`/`u` are §7.4-narrowed by their literal
// initializer, so — same as T5 above — the compound form is now admitted,
// and it inherits §6.6's checked write-back exactly as the non-nullable form
// does (T12): `byte(260)` and unsigned `5u - 10` are both out of range and
// raise `OverflowException` at RUNTIME rather than being rejected at compile
// time. (Confirmed empirically against the non-nullable twins, which already
// raise the same exception — the nullable-narrowed path now agrees.)
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, checked_writeback_narrowed_accepted_3830) {
    BEZEL_EXPECT_EQ(throwsOverflowPlain(
        "var y: byte? := 250; y += 10;"), 1);
    BEZEL_EXPECT_EQ(throwsOverflowPlain(
        "var u: uint? := 5; u -= 10;"), 1);
}

// ===========================================================================
// T11 — §7.4 NARROWED spelling through a PROPERTY. Unchanged in verdict and
// now for a reason that does not depend on issue 3686: §7.4 narrows a local, a
// parameter and a `readonly` field — never a property, whose getter could
// return a different value on the second call (the TOCTOU hazard §7.4's
// readonly rule exists to exclude). So `b.NP` is a nullable operand inside the
// guard exactly as it is outside it.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_property_target_still_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedBody(
        "    using var b := new Box2888(); "
        "    b._n := 40; "
        "    if b.NP <> null then b.NP += 1; end; "
        "    return (b.NP ?? -100); "));
    BEZEL_EXPECT_TRUE(rejectedBody(
        "    using var b := new Box2888(); "
        "    b._n := 40; "
        "    if b.NP <> null then b.NP++; end; "
        "    return (b.NP ?? -100); "));
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

// ===========================================================================
// T13 — issue #3924: a §7.4-NARROWED nullable RHS on a NON-nullable TARGET.
//
// #3830 (T5/T10 above) made §6.6/§17.6 read a compound assignment's operand
// type AFTER §7.4 narrowing — correctly. That sema change admits a codegen
// shape nothing had built for: the TARGET is a plain non-nullable
// scalar/`decimal`, and the RIGHT operand is a DIFFERENT narrowed-nullable
// local/parameter. Narrowing is purely a static/sema fact (§7.4) — it does
// NOT change the narrowed operand's *storage*, which stays the declared
// `{i8 hasValue; T payload}` tagged struct. `Emitter::emitCompoundOp`
// unwrapped that struct only when the LEFT (target) operand already had the
// tagged shape (the #2888/#7.11 lifting arm above); a non-nullable target
// skipped it entirely, so the still-tagged struct flowed into
// `llvm.sadd.with.overflow`/`CreateFAdd` — measured, an IR-verifier abort
// ("Call parameter type does not match function signature!" for the scalar
// family) — or, for `decimal`, was handed whole to `emitDecimalArith`, which
// blindly stores it into a bare-`%bz.decimal`-shaped alloca: a SILENT WRONG
// VALUE with no diagnostic (measured: `d += e` for `decimal? e := 2m` printed
// `11874255106`, not `3`).
//
// THE FIX: `unwrapNarrowedNullableOperand` (already used to unwrap a §7.4-
// narrowed nullable ARGUMENT/INDEX) now also runs on the compound-assignment
// RHS — at the local/field/static call site (LlvmBackend_Stmts.cpp), inside
// `emitCompoundOpTyped` (the ONE function every accessor-mediated caller —
// block-form property, virtual/interface property, imported indexer — already
// funnels through, per #2904/#2888), and at the two heap-collection indexer
// call sites (`lowerCollectionIndexerCompound`'s Array<T> and Dictionary<K,V>
// branches) that call `emitCompoundOp` directly. Every row below is a
// CORRECT VALUE (or, for the decimal rows, the specific value the miscompile
// got wrong), not merely "it compiled" — the whole point of the bug is a
// program that compiles clean and computes garbage.
// ===========================================================================

namespace {

// A full program (not the body-only `runPlain` shape) that prints ONE
// decimal value via `Console.WriteLine` and returns 0 — decimal does not fit
// the 8-bit exit status (see the #2883 file this mirrors).
std::string decimalOut3924(const std::string& body) {
    return std::string(
        "module MD3924; public class CD3924 "
        "  public static method Main(): int ") +
        body + "  end Main; end CD3924;";
}

}  // namespace

// T13.1 — the issue's own two repros, both scalar-integer (IR-verifier abort
// pre-fix) and decimal (silent miscompile pre-fix), target-vs-RHS SAME type.
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_rhs_nonnullable_target_repro_3924) {
    // Repro 1 (§6.6/§7.11, v0.288): int target, int? narrowed RHS, `+=`.
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 5; var m: int? := 3; n += m; return n; "), 8);
    // Repro 2 (§6.4/§6.6, v0.288): decimal target, decimal? narrowed RHS,
    // `+=` — the SILENT-MISCOMPILE row. Pre-fix this printed a nonsense value
    // (11874255106, measured) with NO diagnostic; post-fix it must be exactly
    // the value §6.4 defines, 1.0 + 2.0 = 3.
    BEZEL_EXPECT_EQ(runAotOut(decimalOut3924(
        "    var d: decimal := 1.0m; var e: decimal? := 2.0m; d += e; "
        "    Console.WriteLine(d); return 0; ")), "3");
}

// T13.2 — the exact two programs from fix #3830's own commit message
// (f89b490), whose comment claimed (without a live test — both were
// converted to the parameter form before that commit, so nothing shipped
// broken) that they "compute 8" / "compute 3". Re-reviewed for #3924: BOTH
// crash with the IR-verifier-abort family on the tree #3830 lands against,
// because #3830 is sema-only ("No codegen change was needed", per that
// commit) and never touched `emitCompoundOp`'s target-shape-keyed unwrap.
// Pinned here at their ORIGINAL (un-parameterized, narrowed-local) source
// text, taken from git history's
// tests/lit/sema/compound_nullable_operand_2153__{int_plus_intq,
// int_and_longq}_rejected.bzl at 45c7d71 (pre-#3830).
BEZEL_TEST(codegen_nullable_property_compound_2888, commit_3830_claimed_values_3924) {
    // int_plus_intq: `var i: int := 5; var n: int? := 3; i += n;` — §5.1
    // same-type `+=`.
    BEZEL_EXPECT_EQ(runPlain(
        "    var i: int := 5; var n: int? := 3; i += n; return i; "), 8);
    // int_and_longq: `var i: int := 7; var w: long? := 3L; i &= w;` — §6.2's
    // WIDENING bitwise promotion (int target, WIDER long? narrowed RHS):
    // int widens to long (§5.1), `7L & 3L = 3L`, checked-narrows back to
    // `int` (§6.6/§5.2) — in range, so no OverflowException.
    BEZEL_EXPECT_EQ(runPlain(
        "    var i: int := 7; var w: long? := 3L; i &= w; return i; "), 3);
}

// T13.3 — every arithmetic/bitwise/shift compound operator (§6.6's full list,
// A.6) on an `int` target with a narrowed `int?` RHS local. 10 OP 3 (or 1
// OP 3 for the shifts) for every operator §6.6 admits in compound form.
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_rhs_every_operator_3924) {
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 10; var m: int? := 3; n += m; return n; "), 13);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 10; var m: int? := 3; n -= m; return n; "), 7);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 10; var m: int? := 3; n *= m; return n; "), 30);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 10; var m: int? := 3; n /= m; return n; "), 3);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 10; var m: int? := 3; n mod= m; return n; "), 1);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 6; var m: int? := 3; n &= m; return n; "), 2);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 6; var m: int? := 3; n |= m; return n; "), 7);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 6; var m: int? := 3; n ^= m; return n; "), 5);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 1; var m: int? := 3; n <<= m; return n; "), 8);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: int := 16; var m: int? := 2; n >>= m; return n; "), 4);
}

// T13.4 — the scalar family §6.6 admits (byte/short/long/float/double/char),
// same-type target + narrowed-nullable RHS, `+=`.
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_rhs_scalar_family_3924) {
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: byte := 5; var m: byte? := 3; n += m; return int(n); "), 8);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: short := 5; var m: short? := 3; n += m; return int(n); "),
        8);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: long := 5; var m: long? := 3; n += m; return int(n); "),
        8);
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: char := char(65); var m: char? := char(2); n += m; "
        "    return int(n); "), 67);
    // float/double: not integer exit-status-safe, use the stdout route.
    BEZEL_EXPECT_EQ(runAotOut(decimalOut3924(
        "    var n: float := 5.0f; var m: float? := 3.0f; n += m; "
        "    Console.WriteLine(n); return 0; ")), "8");
    BEZEL_EXPECT_EQ(runAotOut(decimalOut3924(
        "    var n: double := 5.0; var m: double? := 3.0; n += m; "
        "    Console.WriteLine(n); return 0; ")), "8");
}

// T13.5 — §5.1 WIDENING: the target is WIDER than the narrowed RHS's own
// type, so the fix's unwrap must hand a scalar of the RHS's OWN width to
// `emitCompoundOp`'s existing width-promotion tail (§6.2/#2140/#1042) rather
// than the target's. `long += int?` (5 + 3 = 8) and `decimal += int?`
// (1 + 4 = 5, §5.1 int-to-decimal widening, §6.4).
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_rhs_widening_target_3924) {
    BEZEL_EXPECT_EQ(runPlain(
        "    var n: long := 5; var m: int? := 3; n += m; return int(n); "), 8);
    BEZEL_EXPECT_EQ(runAotOut(decimalOut3924(
        "    var d: decimal := 1.0m; var n: int? := 4; d += n; "
        "    Console.WriteLine(d); return 0; ")), "5");
}

// T13.6 — the `decimal` target's full §6.4 operator set with a narrowed
// `decimal?` RHS, mirroring #2883's value-asserted style (`runAotOut`, not
// merely "it compiled").
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_rhs_decimal_every_operator_3924) {
    BEZEL_EXPECT_EQ(runAotOut(decimalOut3924(
        "    var d: decimal := 5.0m; var e: decimal? := 2.0m; d -= e; "
        "    Console.WriteLine(d); return 0; ")), "3");
    BEZEL_EXPECT_EQ(runAotOut(decimalOut3924(
        "    var d: decimal := 5.0m; var e: decimal? := 2.0m; d *= e; "
        "    Console.WriteLine(d); return 0; ")), "10");
    BEZEL_EXPECT_EQ(runAotOut(decimalOut3924(
        "    var d: decimal := 6.0m; var e: decimal? := 2.0m; d /= e; "
        "    Console.WriteLine(d); return 0; ")), "3");
    BEZEL_EXPECT_EQ(runAotOut(decimalOut3924(
        "    var d: decimal := 7.0m; var e: decimal? := 2.0m; d mod= e; "
        "    Console.WriteLine(d); return 0; ")), "1");
}

// T13.7 — the RHS narrowed via an in-body null-check GUARD on a method
// PARAMETER, not a literal-initialized local — §7.4 narrows a parameter
// identically to a local (a local's literal initializer establishes non-null
// immediately; a parameter needs an explicit guard, since the caller could
// pass `null`).
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_rhs_parameter_guard_3924) {
    auto r = runAot(
        "module MPar3924; public class CPar3924 "
        "  public static method Foo(m: int?): int "
        "    var n: int := 5; "
        "    if m <> null then n += m; end if; "
        "    return n; "
        "  end Foo; "
        "  public static method Main(): int "
        "    return Foo(3); "
        "  end Main; "
        "end CPar3924;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 8);
}

// T13.8 — a genuinely-nullable (UN-narrowed) RHS stays REJECTED at sema, the
// control for every row above: a call result is never §7.4-narrowable (its
// value could differ across calls), so `n += Get()` for `Get(): int?` must
// still be a §6.6/§7.11 compile error, exactly as before #3830.
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_rhs_control_call_result_still_rejected_3924) {
    BEZEL_EXPECT_TRUE(rejected2888(
        "module MCall3924; public class CCall3924 "
        "  public static method Get(): int? "
        "    return 3; "
        "  end Get; "
        "  public static method Main(): int "
        "    var n: int := 5; "
        "    n += Get(); "
        "    return n; "
        "  end Main; "
        "end CCall3924;"));
}

// T13.9 — the ACCESSOR-MEDIATED shapes: a block-form `int` property (through
// `emitCompoundOpTyped`, the fix's central call site), a `virtual`/`override`
// property (confirms the fix does not disturb vtable dispatch — 100 + 4 =
// 104, the OVERRIDE's backing field, not the base's), an `Array<int>`
// indexer, and a `Dictionary<string,int>` indexer (the two direct
// `emitCompoundOp` call sites in `lowerCollectionIndexerCompound`) — each
// with a narrowed `int?` RHS local.
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_rhs_accessor_mediated_3924) {
    // Block-form property.
    {
        auto r = runAot(
            "module MProp3924; public class BProp3924 "
            "  private _np: int := 0; "
            "  public property NP: int "
            "    get => _np; "
            "    set => do _np := value; end; "
            "  end NP; "
            "end BProp3924; "
            "public class CProp3924 "
            "  public static method Main(): int "
            "    var b := new BProp3924(); "
            "    var m: int? := 4; "
            "    b.NP += m; "
            "    return b.NP; "
            "  end Main; "
            "end CProp3924;");
        BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 4);
    }
    // Virtual/override property dispatch.
    {
        auto r = runAot(
            "module MVProp3924; public class VBase3924 "
            "  private _v: int := 0; "
            "  public virtual property VP: int "
            "    get => _v; "
            "    set => do _v := value; end; "
            "  end VP; "
            "end VBase3924; "
            "public class VDer3924: VBase3924 "
            "  private _w: int := 100; "
            "  public override property VP: int "
            "    get => _w; "
            "    set => do _w := value; end; "
            "  end VP; "
            "end VDer3924; "
            "public class CVProp3924 "
            "  public static method Main(): int "
            "    var b: VBase3924 := new VDer3924(); "
            "    var m: int? := 4; "
            "    b.VP += m; "
            "    return b.VP; "
            "  end Main; "
            "end CVProp3924;");
        BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 104);
    }
    // Array<int> indexer (lowerCollectionIndexerCompound's Array branch).
    {
        auto r = runAot(
            "module MArr3924; public class CArr3924 "
            "  public static method Main(): int "
            "    var a := new Array<int>(3, 0); "
            "    a[0] := 5; "
            "    var m: int? := 4; "
            "    a[0] += m; "
            "    return a[0]; "
            "  end Main; "
            "end CArr3924;");
        BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 9);
    }
    // Dictionary<string,int> indexer (lowerCollectionIndexerCompound's Dict
    // branch).
    {
        auto r = runAot(
            "module MDict3924; public class CDict3924 "
            "  public static method Main(): int "
            "    var d := new Dictionary<string, int>(); "
            "    d[\"k\"] := 5; "
            "    var m: int? := 4; "
            "    d[\"k\"] += m; "
            "    return d[\"k\"]; "
            "  end Main; "
            "end CDict3924;");
        BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 9);
    }
}

// T13.10 — the `decimal` twin of T13.9's accessor-mediated sweep: a
// block-form `decimal` property and an `Array<decimal>` indexer, each with a
// narrowed `decimal?` RHS. These are the SILENT-MISCOMPILE shape (not an
// abort) through the accessor paths, so the value assertion is load-bearing.
BEZEL_TEST(codegen_nullable_property_compound_2888, narrowed_rhs_decimal_accessor_mediated_3924) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module MDProp3924; public class BDProp3924 "
        "  private _d: decimal := 1.0m; "
        "  public property D: decimal "
        "    get => _d; "
        "    set => do _d := value; end; "
        "  end D; "
        "end BDProp3924; "
        "public class CDProp3924 "
        "  public static method Main(): int "
        "    var b := new BDProp3924(); "
        "    var e: decimal? := 2.0m; "
        "    b.D += e; "
        "    Console.WriteLine(b.D); "
        "    return 0; "
        "  end Main; "
        "end CDProp3924;"), "3");
    BEZEL_EXPECT_EQ(runAotOut(
        "module MDArr3924; public class CDArr3924 "
        "  public static method Main(): int "
        "    var a := new Array<decimal>(1, 0.0m); "
        "    a[0] := 1.0m; "
        "    var e: decimal? := 2.0m; "
        "    a[0] += e; "
        "    Console.WriteLine(a[0]); "
        "    return 0; "
        "  end Main; "
        "end CDArr3924;"), "3");
}
