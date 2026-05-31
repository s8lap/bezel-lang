// Codegen slice CG29c — record `with`-expressions (§9.5, non-destructive
// update). `r with name := value, ... end` evaluates to a copy of the record
// `r` with the named fields replaced; `r` itself is unchanged (records are
// value types). Observable via the int exit code.
//
// Regression: before this slice the `with` path hung the compiler at codegen
// (#254) — the source record value was used directly as a GEP base instead of
// being spilled to a stack slot first.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §9.5: a single named field is replaced; the other is carried from the source.
BEZEL_TEST(codegen_cg29c, with_updates_one_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(1, 2); var b := a with x := 9 end; "
        "  return b.x * 10 + b.y; "
        "end Main; end C;"), 92);
}

// §9.5: `with` is non-destructive — the source record is unchanged.
BEZEL_TEST(codegen_cg29c, with_does_not_mutate_source) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(1, 2); var b := a with x := 9 end; "
        "  return a.x * 10 + a.y; "
        "end Main; end C;"), 12);
}

// Multiple fields replaced in one `with`.
BEZEL_TEST(codegen_cg29c, with_updates_multiple_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; record T(a: int, b: int, c: int); "
        "class C public static method Main(): int "
        "  var t := new T(1, 2, 3); var u := t with a := 7, c := 8 end; "
        "  return u.a * 100 + u.b * 10 + u.c; "
        "end Main; end C;"), 728);
}

// The replacement value may be an arbitrary expression.
BEZEL_TEST(codegen_cg29c, with_value_is_an_expression) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(5, 6); var b := a with x := a.x + a.y end; "
        "  return b.x; "
        "end Main; end C;"), 11);
}

// `with` applied directly to a freshly-constructed record.
BEZEL_TEST(codegen_cg29c, with_on_temporary) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var b := new P(1, 2) with y := 5 end; "
        "  return b.x * 10 + b.y; "
        "end Main; end C;"), 15);
}

// A second `with` on the result of a `with` (chained, all by value).
BEZEL_TEST(codegen_cg29c, chained_with) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new P(1, 2); "
        "  var b := (a with x := 5 end) with y := 6 end; "
        "  return b.x * 10 + b.y; "
        "end Main; end C;"), 56);
}
EOF
echo "written lines=$(wc -l < tests/codegen/record_with_test.cpp)"