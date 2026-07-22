// Codegen tests for nested record field access (§9, §9.1, §9.2, §9.4, §9.5).
//
// A record is a by-value struct (§9.4). When a record holds a record-typed
// field, that inner record is stored INLINE (§9.1 "fields in declaration
// order"; no heap indirection). Accessing an inner record field — whether from
// outside a method (o.Data.Val) or from inside a record method via the implicit
// receiver (Data.Val inside Outer.GetVal) — must GEP through the correct nested
// offsets.
//
// This file is the spec-derived regression suite for the bug where a record
// method reading a RECORD-TYPED field (accessed via the implicit `this` pointer)
// returned 0 instead of the field's actual value. §9.2 states that positional
// parameters become read-only properties accessible as `rec.field`; §9.4 states
// that a record method receives `this` as a pointer to the value-type struct.
// The fix is in the field-address lowering path (LlvmBackend_Layout.cpp /
// LlvmBackend_Exprs.cpp).
//
// Control cases (a) through (f) as specified in the bug report.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
// 8-bit exit channel (migration plan §3): a Main returning a value outside
// 0..255 cannot round-trip the exit code (WEXITSTATUS masks it), so the program
// PRINTS the value (Console.WriteLine, stdlib §2.1) and the test checks the
// captured stdout (JitResult::stdoutText). WriteLine appends '\n'.
std::string runOut(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
} // namespace

// (a) §9.2 / §9.4 — repro: a record method reading a record-typed field via
// the implicit receiver should return the field's value, not 0.
// `Outer.GetVal()` reads `Data.Val` where `Data: Inner` is a positional field.
BEZEL_TEST(codegen_nested_record_field, record_method_reads_nested_record_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(Val: int); "
        "record Outer(Data: Inner) "
        "  public method GetVal(): int => Data.Val; "
        "end Outer; "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(7)); "
        "  return o.GetVal(); "
        "end Main; end C;"),
        7);
}

// (b) §9.1 / §9.4 — two levels of nesting: record holding a record holding a
// record. The outermost method reads through two nested fields.
BEZEL_TEST(codegen_nested_record_field, three_level_nesting_record_method) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record L0(v: int); "
        "record L1(inner: L0) "
        "  public method Get(): int => inner.v; "
        "end L1; "
        "record L2(mid: L1) "
        "  public method Get(): int => mid.inner.v; "
        "end L2; "
        "class C public static method Main(): int "
        "  var a := new L2(new L1(new L0(42))); "
        "  return a.Get(); "
        "end Main; end C;"),
        42);
}

// (c) §9.1 / §9.2 — a record method reads BOTH a record-typed field and a
// primitive field; both should return correct values. Result (305) exceeds the
// 8-bit exit range → observed on stdout.
BEZEL_TEST(codegen_nested_record_field, record_method_reads_record_and_primitive_fields) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "record Tag(code: int); "
        "record Item(label: Tag, count: int) "
        "  public method Key(): int => label.code * 100 + count; "
        "end Item; "
        "class C public static method Main(): int "
        "  var it := new Item(new Tag(3), 5); "
        "  Console.WriteLine(it.Key()); return 0; "
        "end Main; end C;"),
        "305\n");
}

// (d) §9.2 — record-typed field accessed from OUTSIDE a method (`o.Data.Val`)
// without going through a record method body. Verifies the non-method
// field-chaining path is also correct.
BEZEL_TEST(codegen_nested_record_field, nested_record_field_accessed_from_outside) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(Val: int); "
        "record Outer(Data: Inner); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(13)); "
        "  return o.Data.Val; "
        "end Main; end C;"),
        13);
}

// (e) §9.5 — a record method that reads a record-typed field after a `with`
// expression update. The `with` produces a modified copy; the original is
// unchanged.
BEZEL_TEST(codegen_nested_record_field, record_method_after_with_update) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(Val: int); "
        "record Outer(Data: Inner, extra: int) "
        "  public method GetVal(): int => Data.Val; "
        "end Outer; "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(7), 0); "
        "  var o2 := o with(extra := 99); "
        "  return o2.GetVal(); "
        "end Main; end C;"),
        7);
}

// (f) §9.4 — CONTROL: a record method reading a primitive (int) field still
// works correctly. This is the baseline that pre-existed the bug; it must not
// regress.
BEZEL_TEST(codegen_nested_record_field, control_record_method_reads_primitive_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(x: int, y: int) "
        "  public method Sum(): int => x + y; "
        "end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(10, 3); "
        "  return p.Sum(); "
        "end Main; end C;"),
        13);
}

// Additional: nested record field accessed from outside AND through a static
// method parameter (exercises the "record passed by value" path).
BEZEL_TEST(codegen_nested_record_field, nested_field_through_method_param) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Inner(Val: int); "
        "record Outer(Data: Inner); "
        "class C "
        "  public static method ReadVal(o: Outer): int => o.Data.Val; "
        "  public static method Main(): int "
        "    var o := new Outer(new Inner(21)); "
        "    return ReadVal(o); "
        "  end Main; "
        "end C;"),
        21);
}
