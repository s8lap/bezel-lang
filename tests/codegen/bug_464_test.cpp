// Fix verification tests for bugs #464.
//
// #464-A (§7.5 / §7.1 / null-conditional value-field lift through `??`):
//   `(b?.x) ?? fallback` where `b: Point?` (a nullable RECORD) returns the
//   fallback even when `b` is non-null, discarding the actual field value.
//
//   Root cause (confirmed by test): `resolveNullCondMember` in
//   ExprTypecheck_Calls.cpp resolves the member type from the FieldSymbol's
//   AST node. For a class-backed or FieldDecl-backed field it uses
//   `dynamic_cast<const ast::FieldDecl*>(fs->decl)`, which succeeds. But for a
//   POSITIONAL RECORD field (e.g., Point.x declared as `record Point(x: int)`),
//   the FieldSymbol's `decl` is an `ast::Param`, not an `ast::FieldDecl`. The
//   FieldDecl cast returns null, `memberTy` stays null, and the function returns
//   early without recording `int?` as the type of the NullCondMemberExpr node.
//
//   Consequence: `ctx_.exprType(b?.x)` returns null in `lowerCoalesce`. The
//   nullable-value branch (`isNullableValueTy(lt)`) is guarded by that type
//   check; when the type is absent, the guard fails and the function returns
//   `nullptr` — the coalesce is silently dropped, and the BinaryExpr defaults
//   to 0.  (The `?.` lowering itself is correct — `b?.x → {i8,i32}` with the
//   right hasValue and field — but the `??` consumer cannot see this.)
//
//   Fix (src/sema/ExprTypecheck_Calls.cpp, resolveNullCondMember):
//   Mirror the positional-field case already present in resolveMemberAccess:
//   when `fs->posParam && fs->posParam->type`, use the Param's type as the
//   memberTy. This records `int?` (the §7.5-lifted nullable result) correctly,
//   and lowerCoalesce can proceed normally. Per §9 / §7.5.
//
// #464-B (§5 / §9 LINQ Distinct over record array → Count() returns 0):
//   `a.Distinct().Count()` where `a: Array<Point>` (record element type).
//   INVESTIGATION: this scenario was already handled correctly by the time this
//   PR landed. The Distinct-over-record trampoline (buildLinqTrampoline kind=3
//   with emitStructEquality) and the lazy terminal Count materialisation path
//   both work. The tests below pass on the base as well; they are included as
//   regression guards confirming the scenario is correct.
//
//   Spec: §5 LINQ operators, §7.5 null-conditional, §9 records, §9.1 equality.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── #464-A: (b?.x) ?? fallback on a nullable RECORD receiver ─────────────────

// §7.5 / §7.1 / #464: `(b?.x) ?? 0` where `b: Point? = new Point(7, 9)`.
// Per spec §7.5 the null-conditional result is the field value (7) when the
// receiver is non-null. Per §7.1 `??` returns the left operand when non-null.
// Expected: 7. Before fix: 0 (the fallback — hasValue was treated as false).
BEZEL_TEST(bug464_nullcond_value_field_coalesce,
           nullable_record_nonnull_yields_field_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var b: Point? := new Point(7, 9); "
        "  return (b?.x) ?? 0; "   // expect 7
        "end Main; end C;"), 7);
}

// §7.5 / #464: null receiver → `??` returns the fallback. Regardless of the
// fix, a null receiver must produce the fallback (0). Regression guard.
BEZEL_TEST(bug464_nullcond_value_field_coalesce,
           nullable_record_null_yields_fallback_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var b: Point? := null; "
        "  return (b?.x) ?? 0; "   // expect 0 (fallback)
        "end Main; end C;"), 0);
}

// §7.5 / #464: `b?.x` assigned directly to `int?` (without `??`), non-null b.
// This tests the lift itself: the int? result must be non-null with hasValue=1
// and the actual field value. Expected: x! = 7.
BEZEL_TEST(bug464_nullcond_value_field_coalesce,
           nullable_record_direct_assign_int_nullable_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var b: Point? := new Point(7, 9); "
        "  var v: int? := b?.x; "
        "  if v = null then return 200; end; "
        "  return v!; "   // expect 7
        "end Main; end C;"), 7);
}

// §7.5 / #464: `b?.y` (second field) with a non-null receiver. Exercises
// the field-index path for a field that is NOT field 0.
BEZEL_TEST(bug464_nullcond_value_field_coalesce,
           nullable_record_second_field_coalesce_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var b: Point? := new Point(7, 9); "
        "  return (b?.y) ?? 0; "   // expect 9
        "end Main; end C;"), 9);
}

// §7.5 / #464: both fields, non-null receiver, using the two results.
// (b?.x) ?? 0 + (b?.y) ?? 0 = 7 + 9 = 16.
BEZEL_TEST(bug464_nullcond_value_field_coalesce,
           nullable_record_both_fields_coalesce_sum_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var b: Point? := new Point(7, 9); "
        "  var rx: int := (b?.x) ?? 0; "
        "  var ry: int := (b?.y) ?? 0; "
        "  return rx + ry; "   // 7 + 9 = 16
        "end Main; end C;"), 16);
}

// §7.5 / #464: regression — a CLASS receiver's `?.field ?? fallback` must still
// work (the pointer-receiver path is unchanged by the fix).
// n: N (class with V:int), n?.V ?? 99 must yield V when n non-null.
BEZEL_TEST(bug464_nullcond_value_field_coalesce,
           regression_class_receiver_coalesce_unaffected_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N public V: int := 0; end N; "
        "class C public static method Main(): int "
        "  var n := new N(); n.V := 42; "
        "  var r: int := n?.V ?? 99; "   // expect 42
        "  return r; "
        "end Main; end C;"), 42);
}

// ── #464-B: Distinct().Count() over a record array ───────────────────────────

// §5 / §9 / #464: `a.Distinct().Count()` over `Array<Point>` with 2 duplicates
// and 1 unique. Expected distinct count: 2. Before fix: 0.
BEZEL_TEST(bug464_distinct_record_array,
           distinct_record_array_count_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(1, 2)); "   // duplicate
        "  a.Add(new Point(3, 4)); "
        "  return a.Distinct().Count(); "   // expect 2
        "end Main; end C;"), 2);
}

// §5 / §9 / #464: `a.Distinct().Count()` where ALL elements are distinct.
// Expected: 3 (all three survive dedup).
BEZEL_TEST(bug464_distinct_record_array,
           distinct_record_array_all_unique_count_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(3, 4)); "
        "  a.Add(new Point(5, 6)); "
        "  return a.Distinct().Count(); "   // expect 3
        "end Main; end C;"), 3);
}

// §5 / §9 / #464: `a.Distinct().Count()` where ALL elements are identical.
// Expected: 1.
BEZEL_TEST(bug464_distinct_record_array,
           distinct_record_array_all_same_count_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(1, 2)); "
        "  return a.Distinct().Count(); "   // expect 1
        "end Main; end C;"), 1);
}

// §5 / #464: regression — Distinct().Count() over int array must still work
// (the non-record path is unaffected by the fix).
BEZEL_TEST(bug464_distinct_record_array,
           regression_distinct_int_count_unaffected_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(1); a.Add(3); "
        "  return a.Distinct().Count(); "   // expect 3
        "end Main; end C;"), 3);
}

// §5 / §9 / #464: Distinct elements' actual values are correct (not just count).
// Verify by summing x-fields of the distinct elements via ToArray + index access.
BEZEL_TEST(bug464_distinct_record_array,
           distinct_record_array_values_correct_464) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  a.Add(new Point(1, 2)); "
        "  a.Add(new Point(1, 2)); "   // duplicate — should not appear twice
        "  a.Add(new Point(3, 4)); "
        "  using var d: Array<Point> := a.Distinct().ToArray(); "
        "  if d.Count <> 2 then return -1; end; "
        "  var sum := d[0].x + d[1].x; "   // 1 + 3 = 4
        "  return sum; "
        "end Main; end C;"), 4);
}
