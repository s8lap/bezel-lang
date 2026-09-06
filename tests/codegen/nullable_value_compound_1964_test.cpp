// Codegen — #1964 (§7.11 / §6.6 / §5.2): a compound assignment whose LHS is a
// nullable VALUE type (`char?`, `int?`, `long?`, `double?`, …) must lift per
// §7.11, not crash codegen with malformed IR.
//
// The target slot is the tagged `{i8 hasValue; T}` struct. Before the fix the
// compound-assignment path loaded that struct and fed it straight to the
// built-in emitCompoundOp, which passed the WHOLE struct to
// `llvm.*add.with.overflow` — emitting `llvm.uadd.with.overflow.sl_i8i32s`
// (an i32-payload/i8-tag struct where an integer was required) and aborting the
// IR verifier ("Intrinsic has incorrect return type"). The exact repro from the
// issue — `var c: char? := char('A'); c += 1;` — crashed the compiler.
//
// ⚠ THE RULE CHANGED AT v0.267 (proposal 0114, #3298), and this file is now
// the REJECTION side of it. #1964 landed at v0.214 against a §7.11 that lifted:
// "the arithmetic/bitwise/shift operators lift to `T?` — the result is `null`
// if EITHER operand is `null`". §7.11 (v0.286) opens "No operator **lifts** to
// a nullable type", and §6.6 makes a read-modify-write compound "a **compile
// error** when the static type of its left-hand side, or of its right operand,
// is a **nullable** type". So the crash #1964 fixed is not merely fixed, the
// form is gone: nothing reaches emitCompoundOp with a tagged struct any more,
// because sema rejects the statement first.
//
// Every row below therefore asserts the DIAGNOSIS, and pairs it with the
// NON-NULLABLE twin of the same statement so the rule each row actually exists
// for — §6.6's checked narrow-integer write-back, §5.2's `char` code-point
// check, §6.1's `OverflowException`/`DivideByZeroException`, the unsigned
// no-false-overflow case, the `long` width promotion — keeps its value
// coverage rather than being deleted along with the lifting. That pairing is
// the point of the rewrite: the crash class #1964 names lives in
// `emitCompoundOp`, which the non-nullable twins still exercise.
//
// §7.4 DOES now rescue a NARROWED target (fix #3830, v0.288, ruling on issue
// 3686): narrowing reaches compound assignment exactly as it already reached
// the binary form, so `var c: char? := char('A'); c += 1;` — narrowed by its
// non-nullable-typed initializer, §7.4 — no longer hits this file's gate at
// all (verified empirically: it compiles and computes 'B'). Every row below
// that used a literal/explicit-conversion initializer for its "rejected"
// assertion has been rewritten to pass that same value through a PARAMETER
// of a small `Chk` helper instead (`rejectedCompoundP`) — a parameter is
// never narrowed by what the caller passes, narrowing requires an in-body
// null test — so each row keeps testing a genuinely nullable, UNGUARDED
// operand, which is what this file's subject (the declared-type gate with no
// narrowing in scope at all) actually needs. The pre-existing `:= null`
// twins were never narrowed and are untouched. The NARROWED-accepted side —
// this file's own repro, `char? += 1` on a narrowed `char?`, computing 'B' —
// is pinned with asserted VALUES in
// codegen_nullable_property_compound_2888.local_increment_narrowed_accepted_3830
// and .checked_writeback_narrowed_accepted_3830
// (tests/codegen/nullable_property_compound_2888_test.cpp).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Body-only program returning an int exit code (result fits 0..255).
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// Returns 1 if `body` raises OverflowException, 0 if it runs to completion,
// -999 on a front-end failure.
int throwsOverflow(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") +
        body +
        " return 0; catch OverflowException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}


// A rejection whose diagnostic is §6.6's, not merely "did not run".
bool rejectedCompound(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("compound assignment") == std::string::npos) continue;
        if (m.find("nullable") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        return true;
    }
    return false;
}

// fix #3830 (v0.288): the PARAMETER twin of `rejectedCompound`, for a row
// whose "still rejected" operand must NOT be §7.4-narrowed. A local declared
// `var x: T? := <literal-or-explicit-conversion>;` narrows at its
// initializer (an int/float/char/string/bool literal, or any expression
// whose own static type is non-nullable — `char(n)` included), and #3830
// makes such a narrowed operand admitted at compound assignment exactly as
// at the binary form — so a row meant to test the genuinely-nullable,
// UNGUARDED gate must route its value through a PARAMETER instead: a
// parameter is never narrowed by what the caller passes (narrowing needs an
// in-body null test, §7.4). `paramDecl` is the sole parameter's declaration
// (e.g. "c: char?"), `argExpr` the value passed at the call site, and `body`
// the helper's statements (referencing the parameter by name). Same
// diagnostic gate as `rejectedCompound`.
bool rejectedCompoundP(const std::string& paramDecl,
                       const std::string& argExpr,
                       const std::string& body) {
    auto r = runAot(
        std::string("module M; class C "
                    "  public static method Chk(") + paramDecl + "): int " +
        body +
        "  end Chk; "
        "  public static method Main(): int return Chk(" + argExpr + "); "
        "end Main; "
        "end C;");
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("compound assignment") == std::string::npos) continue;
        if (m.find("nullable") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// --- The exact issue repro. `char? += 1` was a codegen crash at v0.214, a
// lifted value from #1964 until v0.267, and a §6.6 compile error now. The
// NON-nullable twin keeps §5.2's code-point write-back covered — that is the
// rule the row exists for and it still runs through emitCompoundOp. ---
BEZEL_TEST(codegen_1964_nullable_compound, char_plus_equals_rejected_3298) {
    // fix #3830: `char('A')` is an explicit conversion whose own static type
    // is non-nullable, so a local initialized with it narrows (§7.4) exactly
    // as a bare literal does; routed through a parameter to keep testing the
    // genuinely nullable, unguarded gate.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("c: char?", "char('A')", "c += 1; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound("var c: char? := null; c += 1; return 0;"));
    // §5.2 twin, non-nullable: 'A' + 1 = 'B' (66).
    BEZEL_EXPECT_EQ(
        run("var c: char := char('A'); c += 1; return int(c);"), 66);
}

// §5.2's code-point range check on the non-nullable path (the nullable
// spelling that used to carry it is rejected above).
BEZEL_TEST(codegen_1964_nullable_compound, char_above_max_throws_nonnullable) {
    // fix #3830: parameter, not a narrowed local — see char_plus_equals above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("c: char?", "char(0x10FFFF)", "c += 1; return 0;"));
    BEZEL_EXPECT_EQ(throwsOverflow("var c: char := char(0x10FFFF); c += 1;"), 1);
}

BEZEL_TEST(codegen_1964_nullable_compound, int_plus_equals_rejected_3298) {
    // fix #3830: `5` is an int literal — a local initialized with it narrows
    // (§7.4); routed through a parameter to keep testing the genuinely
    // nullable, unguarded gate.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("i: int?", "5", "i += 10; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound("var i: int? := null; i += 10; return 0;"));
    BEZEL_EXPECT_EQ(run("var i: int := 5; i += 10; return i;"), 15);
}

BEZEL_TEST(codegen_1964_nullable_compound, int_times_and_minus_rejected_3298) {
    // fix #3830: both operands are int literals — parameterized, see above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("i: int?", "6", "i *= 7; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompoundP("i: int?", "50", "i -= 8; return 0;"));
    BEZEL_EXPECT_EQ(run("var i: int := 6; i *= 7; return i;"), 42);
    BEZEL_EXPECT_EQ(run("var i: int := 50; i -= 8; return i;"), 42);
}

// §6.1's DivideByZeroException — §7.11 is explicit that the operators' runtime
// checks are "unchanged and unmoved … reached after the operands are
// narrowed", so the check belongs to the non-nullable form now.
BEZEL_TEST(codegen_1964_nullable_compound, int_div_by_zero_throws_nonnullable) {
    // fix #3830: parameterized — see int_plus_equals above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("i: int?", "10", "i /= 0; return 0;"));
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var i: int := 10; "
        "  try i /= 0; return 0; catch DivideByZeroException => return 1; end; "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

BEZEL_TEST(codegen_1964_nullable_compound, long_width_promotion_nonnullable) {
    // fix #3830: `9000000000L` is an int literal (long-suffixed) —
    // parameterized, see int_plus_equals above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("l: long?", "9000000000L", "l += 1; return 0;"));
    BEZEL_EXPECT_EQ(run(
        "var l: long := 9000000000L; l += 1; "
        "if l = 9000000001L then return 1; end; return 0;"), 1);
}

// §6.6's CHECKED narrow-integer write-back, both directions.
BEZEL_TEST(codegen_1964_nullable_compound, byte_writeback_nonnullable) {
    // fix #3830: both operands are int literals — parameterized, see
    // int_plus_equals above. (A narrowed `byte?` write-back is admitted and
    // raises/computes exactly like the non-nullable form below — pinned
    // with values by codegen_nullable_property_compound_2888
    // .checked_writeback_narrowed_accepted_3830.)
    BEZEL_EXPECT_TRUE(rejectedCompoundP("b: byte?", "200", "b += 100; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompoundP("b: byte?", "100", "b += 50; return 0;"));
    BEZEL_EXPECT_EQ(throwsOverflow("var b: byte := 200; b += 100;"), 1);
    BEZEL_EXPECT_EQ(run("var b: byte := 100; b += 50; return int(b);"), 150);
}

// §6.2 signedness: an unsigned target near its maximum must NOT raise a false
// overflow through the signed intrinsic.
BEZEL_TEST(codegen_1964_nullable_compound, uint_no_false_overflow_nonnullable) {
    // fix #3830: parameterized — see int_plus_equals above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("u: uint?", "4000000000U", "u += 1; return 0;"));
    BEZEL_EXPECT_EQ(throwsOverflow("var u: uint := 4000000000U; u += 1;"), 0);
}

BEZEL_TEST(codegen_1964_nullable_compound, double_plus_equals_rejected_3298) {
    // fix #3830: `10.0` is a float literal — a local initialized with it
    // narrows (§7.4); parameterized, see int_plus_equals above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("d: double?", "10.0", "d += 1; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound("var d: double? := null; d += 1; return 0;"));
    BEZEL_EXPECT_EQ(run("var d: double := 10.0; d += 1; return int(d);"), 11);
}
