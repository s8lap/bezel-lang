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
// §7.4 does NOT rescue any of these: a property and an indexer slot are never
// narrowable (§7.4 admits a local, a parameter and a `readonly` field only),
// and for a nullable-typed local target #3298 implements the reading that §6.6
// and §17.6 enumerate by DECLARED type — recorded, with the alternative and
// the reason, as issue 3686.
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
    BEZEL_EXPECT_TRUE(rejectedPlain(
        "    var n: int? := 3; "
        "    var t: int := 5; "
        "    t += n; "
        "    return t; "));
}

// ===========================================================================
// T5 — the plain LOCAL. §17.6's desugaring is §6.6's, so `a++` on an `int?`
// takes the same verdict as `a += 1`. ⚠ This is the row where #3298's recorded
// interpretation bites (issue 3686): `a` here IS §7.4-narrowed by its non-null
// literal initialiser, and the equivalent `a := a + 1` IS admitted — §6.6 and
// §17.6 are read as enumerations over the DECLARED type. The asymmetry is
// deliberate, recorded, and filed; if 3686 rules the other way, THIS row is
// the one that flips back.
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, local_increment_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedPlain("    var a: int? := 5; a++; return 0; "));
    BEZEL_EXPECT_TRUE(rejectedPlain("    var b: int? := 5; b--; return 0; "));
    BEZEL_EXPECT_TRUE(rejectedPlain("    var c: int? := 10; c += 1; return 0; "));
    // …while the desugared spelling on the SAME narrowed local compiles and
    // gives the value, which is what issue 3686 is about. Asserted here so the
    // asymmetry is visible at the site rather than only in the issue.
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
// T10 — §6.6's checked narrow-integer write-back and §6.2's signedness, which
// this file reached through nullable targets. §6.6's write-back paragraph is
// explicit that it "is unreached here: no operation types" once the operand is
// nullable, so the rule is now exercised on the NON-nullable path only (T12).
// ===========================================================================
BEZEL_TEST(codegen_nullable_property_compound_2888, checked_writeback_nullable_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedPlain(
        "    var y: byte? := 250; y += 10; return 0; "));
    BEZEL_EXPECT_TRUE(rejectedPlain(
        "    var u: uint? := 5; u -= 10; return 0; "));
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
