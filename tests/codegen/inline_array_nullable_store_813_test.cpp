// fix #813 / §15.1: storing into an inline nullable-value array `[N]T?` (element
// assignment `a[i] := v` and literal initialization `[e0, e1]`) must wrap the
// inner value in the nullable slot struct `{i8 hasValue, T value}`. Before the
// fix, the fast paths (IndexExpr assignment, inline-array literal assign, and
// the literal initializer) stored the raw inner bytes, clobbering the hasValue
// byte and leaving the value field zero — so reads returned 0 (or `!` threw on a
// provably-assigned slot). Affects int?, bool?, enum?, and record? elements.
//
// Value (codegen) tests — runJit is the right harness.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §15.1: `[1]int?` element assignment then `!` extraction. 42 (was 0).
BEZEL_TEST(inline_array_nullable_store_813, int_element_assign) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var ps: [1]int?; "
        "    ps[0] := 42; "
        "    return ps[0]!; "
        "  end Main; "
        "end C;"), 42);
}

// §15.1 / §10: `[1]Color?` with a NON-ZERO enum value reads back correctly. 1.
BEZEL_TEST(inline_array_nullable_store_813, enum_element_nonzero) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C "
        "  public static method Main(): int "
        "    var cs: [1]Color?; "
        "    cs[0] := Color.Green; "
        "    return int(cs[0]!); "       // 1
        "  end Main; "
        "end C;"), 1);
}

// §15.1 / §10: a ZERO-valued enum stored into `[1]Color?` is non-null (the
// hasValue byte must be set even though the value is 0); `!` must not throw. 0.
BEZEL_TEST(inline_array_nullable_store_813, enum_element_zero_not_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C "
        "  public static method Main(): int "
        "    var cs: [1]Color?; "
        "    cs[0] := Color.Red; "
        "    return int(cs[0]!); "       // 0, no throw
        "  end Main; "
        "end C;"), 0);
}

// §15.1 / §9: `[2]P?` literal initializer with record elements; fields read back
// at the correct offsets (10+20+30+40). 100 (was 60 — fields shifted).
BEZEL_TEST(inline_array_nullable_store_813, record_literal_init) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); class C "
        "  public static method Main(): int "
        "    var ps: [2]P? := [new P(10, 20), new P(30, 40)]; "
        "    return ps[0]!.x + ps[0]!.y + ps[1]!.x + ps[1]!.y; "   // 100
        "  end Main; "
        "end C;"), 100);
}

// §15.1 / §9: `[1]P?` element assignment with a record value; fields read back
// at the correct offsets. x + y*10 = 10 + 200 = 210 (was 20: x read 20, y 0).
BEZEL_TEST(inline_array_nullable_store_813, record_element_assign) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); class C "
        "  public static method Main(): int "
        "    var ps: [1]P?; "
        "    ps[0] := new P(10, 20); "
        "    return ps[0]!.x + ps[0]!.y * 10; "    // 10 + 200 = 210
        "  end Main; "
        "end C;"), 210);
}

// §15.1: regression guard — a NON-nullable inline record array `[N]P` (whose
// element struct is also a 2-field struct) is unaffected by the nullable-slot
// wrapping. 100.
BEZEL_TEST(inline_array_nullable_store_813, nonnullable_record_array_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); class C "
        "  public static method Main(): int "
        "    var ps: [2]P := [new P(10, 20), new P(30, 40)]; "
        "    return ps[0].x + ps[0].y + ps[1].x + ps[1].y; "       // 100
        "  end Main; "
        "end C;"), 100);
}
