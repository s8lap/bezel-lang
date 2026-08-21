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
// §7.4 does not rescue these: a nullable-TYPED target is rejected by §6.6 as
// an enumeration over the DECLARED type even where a narrowing holds. That
// reading is #3298's recorded interpretation, with its alternative and the
// reason, filed as issue 3686; if 3686 rules the other way, the `present`
// rows here are the ones that flip back.

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

}  // namespace

// --- The exact issue repro. `char? += 1` was a codegen crash at v0.214, a
// lifted value from #1964 until v0.267, and a §6.6 compile error now. The
// NON-nullable twin keeps §5.2's code-point write-back covered — that is the
// rule the row exists for and it still runs through emitCompoundOp. ---
BEZEL_TEST(codegen_1964_nullable_compound, char_plus_equals_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedCompound("var c: char? := char('A'); c += 1; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound("var c: char? := null; c += 1; return 0;"));
    // §5.2 twin, non-nullable: 'A' + 1 = 'B' (66).
    BEZEL_EXPECT_EQ(
        run("var c: char := char('A'); c += 1; return int(c);"), 66);
}

// §5.2's code-point range check on the non-nullable path (the nullable
// spelling that used to carry it is rejected above).
BEZEL_TEST(codegen_1964_nullable_compound, char_above_max_throws_nonnullable) {
    BEZEL_EXPECT_TRUE(rejectedCompound("var c: char? := char(0x10FFFF); c += 1; return 0;"));
    BEZEL_EXPECT_EQ(throwsOverflow("var c: char := char(0x10FFFF); c += 1;"), 1);
}

BEZEL_TEST(codegen_1964_nullable_compound, int_plus_equals_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedCompound("var i: int? := 5; i += 10; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound("var i: int? := null; i += 10; return 0;"));
    BEZEL_EXPECT_EQ(run("var i: int := 5; i += 10; return i;"), 15);
}

BEZEL_TEST(codegen_1964_nullable_compound, int_times_and_minus_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedCompound("var i: int? := 6; i *= 7; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound("var i: int? := 50; i -= 8; return 0;"));
    BEZEL_EXPECT_EQ(run("var i: int := 6; i *= 7; return i;"), 42);
    BEZEL_EXPECT_EQ(run("var i: int := 50; i -= 8; return i;"), 42);
}

// §6.1's DivideByZeroException — §7.11 is explicit that the operators' runtime
// checks are "unchanged and unmoved … reached after the operands are
// narrowed", so the check belongs to the non-nullable form now.
BEZEL_TEST(codegen_1964_nullable_compound, int_div_by_zero_throws_nonnullable) {
    BEZEL_EXPECT_TRUE(rejectedCompound("var i: int? := 10; i /= 0; return 0;"));
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var i: int := 10; "
        "  try i /= 0; return 0; catch DivideByZeroException => return 1; end; "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

BEZEL_TEST(codegen_1964_nullable_compound, long_width_promotion_nonnullable) {
    BEZEL_EXPECT_TRUE(rejectedCompound("var l: long? := 9000000000L; l += 1; return 0;"));
    BEZEL_EXPECT_EQ(run(
        "var l: long := 9000000000L; l += 1; "
        "if l = 9000000001L then return 1; end; return 0;"), 1);
}

// §6.6's CHECKED narrow-integer write-back, both directions.
BEZEL_TEST(codegen_1964_nullable_compound, byte_writeback_nonnullable) {
    BEZEL_EXPECT_TRUE(rejectedCompound("var b: byte? := 200; b += 100; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound("var b: byte? := 100; b += 50; return 0;"));
    BEZEL_EXPECT_EQ(throwsOverflow("var b: byte := 200; b += 100;"), 1);
    BEZEL_EXPECT_EQ(run("var b: byte := 100; b += 50; return int(b);"), 150);
}

// §6.2 signedness: an unsigned target near its maximum must NOT raise a false
// overflow through the signed intrinsic.
BEZEL_TEST(codegen_1964_nullable_compound, uint_no_false_overflow_nonnullable) {
    BEZEL_EXPECT_TRUE(rejectedCompound("var u: uint? := 4000000000U; u += 1; return 0;"));
    BEZEL_EXPECT_EQ(throwsOverflow("var u: uint := 4000000000U; u += 1;"), 0);
}

BEZEL_TEST(codegen_1964_nullable_compound, double_plus_equals_rejected_3298) {
    BEZEL_EXPECT_TRUE(rejectedCompound("var d: double? := 10.0; d += 1; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound("var d: double? := null; d += 1; return 0;"));
    BEZEL_EXPECT_EQ(run("var d: double := 10.0; d += 1; return int(d);"), 11);
}
