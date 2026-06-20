// fix #848 / §8.4: a record that overloads `operator =`/`<>`, compared through a
// nullable wrapper (`T?`) or against the `null` literal, emitted malformed IR —
// codegen passed the `{i8, T}` nullable struct (or `ptr null`) straight to the
// user operator, which takes a bare `T`. Per §8.4, `=`/`<>` is null-safe even for
// a user-overloaded record: `null = null` → true, `null = nonNull` → false, and
// both-present invokes the operator on the unwrapped payloads. The fix wraps the
// operator call in that null-safe protocol, branch-guarding the call so it never
// runs on a null/zero payload (a record with a reference-typed field would
// otherwise dereference a null field).
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
// Point overloading `=`/`<>` (§25.2 requires both together).
const char* kPoint =
    "module M; "
    "record Point(X: int, Y: int) "
    "  public static method operator =(a: Point, b: Point): bool => "
    "    a.X = b.X and a.Y = b.Y; "
    "  public static method operator <>(a: Point, b: Point): bool => "
    "    a.X <> b.X or a.Y <> b.Y; "
    "end Point; ";
}  // namespace

// bug06: `Point? = Point?` — present-vs-null is false, null-vs-null is true (was
// an IR call-param mismatch).
BEZEL_TEST(nullable_record_user_equality_848, nullable_both_sides) {
    BEZEL_EXPECT_EQ(run(std::string(kPoint) +
        "class C public static method Main(): int "
        "  var a: Point? := new Point(1, 2); "
        "  var b: Point? := null; "
        "  var r1: bool := a = b; "       // present vs null → false
        "  var r2: bool := b = b; "       // null vs null → true
        "  if r1 then return 10; end; "
        "  if r2 then return 2; end; "
        "  return 0; "
        "end Main; end C;"), 2);
}

// bug07: comparing a nullable record to the `null` literal uses null-safe
// equality (never the user operator): p=null → false, q=null → true, p<>null →
// true (was an IR mismatch with `ptr null`).
BEZEL_TEST(nullable_record_user_equality_848, compare_to_null_literal) {
    BEZEL_EXPECT_EQ(run(std::string(kPoint) +
        "class C public static method Main(): int "
        "  var p: Point? := new Point(1, 2); "
        "  var q: Point? := null; "
        "  var r1: bool := p = null; "    // false
        "  var r2: bool := q = null; "    // true
        "  var r3: bool := p <> null; "   // true
        "  if not r1 and r2 and r3 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// Both present → the user operator IS invoked on the unwrapped payloads
// (equal payloads → equal; differing payloads → `<>` true).
BEZEL_TEST(nullable_record_user_equality_848, both_present_invokes_operator) {
    BEZEL_EXPECT_EQ(run(std::string(kPoint) +
        "class C public static method Main(): int "
        "  var a: Point? := new Point(5, 5); "
        "  var b: Point? := new Point(5, 5); "
        "  var c: Point? := new Point(9, 9); "
        "  if a = b and a <> c then return 8; end; "
        "  return 0; "
        "end Main; end C;"), 8);
}

// A mixed comparison (non-nullable `Point` vs `Point?`) lifts the non-nullable
// operand as present.
BEZEL_TEST(nullable_record_user_equality_848, mixed_nullable_nonnullable) {
    BEZEL_EXPECT_EQ(run(std::string(kPoint) +
        "class C public static method Main(): int "
        "  var a := new Point(3, 3); "
        "  var b: Point? := new Point(3, 3); "
        "  var c: Point? := null; "
        "  if a = b then return (6 if not (a = c) else 0); end; "
        "  return 0; "
        "end Main; end C;"), 6);
}

// Safety: a record with a reference-typed (string) field overloading `=`/`<>`,
// compared to null, must not dereference the null payload's string field — the
// operator is branch-guarded to the both-present path.
BEZEL_TEST(nullable_record_user_equality_848, reference_field_record_vs_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Named(Name: string, Id: int) "
        "  public static method operator =(a: Named, b: Named): bool => a.Id = b.Id; "
        "  public static method operator <>(a: Named, b: Named): bool => a.Id <> b.Id; "
        "end Named; "
        "class C public static method Main(): int "
        "  var a: Named? := new Named(\"alice\", 1); "
        "  var b: Named? := null; "
        "  if a = b then return 20; end; "        // false (present vs null)
        "  if not (b = b) then return 21; end; "  // b=b is true (null vs null)
        "  return 5; "
        "end Main; end C;"), 5);
}
