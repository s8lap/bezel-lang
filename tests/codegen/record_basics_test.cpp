// Codegen slice CG29 — records as value types (§9), foundational slice.
//
// This first slice covers construction of a positional-form record, reading
// its positional fields (which become public read-only properties, §9.1/9.2),
// passing a record to a method and returning one, and a record body method
// that reads a positional field via the implicit value. Copy-on-assignment,
// structural equality, and `with`-expressions land in later slices.
//
// A record is a by-value LLVM struct (§9.4); these tests observe field values
// through the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §9.1: positional construction binds fields in order; §9.2: positional
// parameters become public read-only properties readable as `p.x`.
BEZEL_TEST(codegen_cg29, construct_and_read_positional_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := Point(3, 4); return p.x * 10 + p.y; "
        "end Main; end C;"), 34);
}

// A record passed to a method is passed by value (§9.4); the callee reads its
// fields.
BEZEL_TEST(codegen_cg29, record_passed_to_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C "
        "  public static method Sum(p: Point): int => p.x + p.y; "
        "  public static method Main(): int "
        "    var p := Point(5, 7); return Sum(p); "
        "  end Main; "
        "end C;"), 12);
}

// A record returned from a method (by value, §9.4).
BEZEL_TEST(codegen_cg29, record_returned_from_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C "
        "  public static method Make(): Point => Point(2, 9); "
        "  public static method Main(): int "
        "    var p := Make(); return p.x + p.y; "
        "  end Main; "
        "end C;"), 11);
}

// §9.1 block form: a record body method reads a positional field on `this`
// (the value).
BEZEL_TEST(codegen_cg29, record_body_method_reads_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Vec(dx: int, dy: int) "
        "  public method Len2(): int => dx * dx + dy * dy; "
        "end Vec; "
        "class C public static method Main(): int "
        "  var v := Vec(3, 4); return v.Len2(); "
        "end Main; end C;"), 25);
}

// A record with three fields; field order in the struct matches declaration.
BEZEL_TEST(codegen_cg29, three_field_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Triple(a: int, b: int, c: int); "
        "class C public static method Main(): int "
        "  var t := Triple(1, 2, 3); return t.a * 100 + t.b * 10 + t.c; "
        "end Main; end C;"), 123);
}

// A record-typed local can be reassigned to a fresh value.
BEZEL_TEST(codegen_cg29, record_local_reassigned) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := Point(1, 1); p := Point(8, 9); return p.x * 10 + p.y; "
        "end Main; end C;"), 89);
}
