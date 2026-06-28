// fix #862 / §7.4: nullable-field narrowing is not allowed — the compiler narrows
// only local variables, never fields or properties. Accessing a member on a
// nullable field without first copying it to a local is a compile error.
// These tests verify:
//   - instance field `<> null` guard then `.Member` → rejected (§7.4)
//   - instance field `is not null` guard then `.Member` → rejected (§7.4)
//   - static nullable field access after null check → rejected (§7.4)
//   - copy the field to a local and narrow the local → accepted (§7.4 safe pattern)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// True iff some diagnostic mentions the §7.4 field-narrowing rule.
bool hasFieldNarrowingError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("§7.4") != std::string::npos)
            return true;
    return false;
}

// Minimal Container class used across test cases.
constexpr const char* kPreamble =
    "module M; "
    "class Container "
    "  public _name: string?; "
    "  public constructor(n: string) _name := n; end constructor; "
    "  destructor end destructor; "
    "end Container; ";

}  // namespace

// §7.4 (#862): accessing a member on a nullable instance FIELD directly after a
// `<> null` guard is a compile error — fields are not narrowable (TOCTOU hazard).
BEZEL_TEST(nullable_field_narrowing_862, instance_field_ne_null_guard_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "class C public static method Main(): int "
        "  using var c := new Container(\"hello\"); "
        "  if c._name <> null then "
        "    return c._name.Length; "  // §7.4: field not narrowable
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldNarrowingError(r));
}

// §7.4 (#862): `is not null` desugars to `<> null` (per §7.4 / §12.7); the
// field-narrowing rejection must fire regardless of which null-guard spelling is
// used.
BEZEL_TEST(nullable_field_narrowing_862, instance_field_is_not_null_guard_rejected) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "class C public static method Main(): int "
        "  using var c := new Container(\"hello\"); "
        "  if c._name is not null then "
        "    return c._name.Length; "  // §7.4: field not narrowable
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldNarrowingError(r));
}

// §7.4 (#862): a static nullable field is likewise un-narrowable.
BEZEL_TEST(nullable_field_narrowing_862, static_field_ne_null_guard_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Counter "
        "  public static _label: string? := \"hello\"; "
        "  public static method Main(): int "
        "    if Counter._label <> null then "
        "      return Counter._label.Length; "  // §7.4: static field not narrowable
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldNarrowingError(r));
}

// §7.4 (safe pattern): copying the nullable field to a local and narrowing the
// local is the correct idiom — it must be ACCEPTED with no errors.
BEZEL_TEST(nullable_field_narrowing_862, copy_to_local_then_guard_accepted) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "class C public static method Main(): int "
        "  using var c := new Container(\"hello\"); "
        "  var n := c._name; "      // copy to a local
        "  if n <> null then "
        "    return n.Length; "     // §7.4: local IS narrowable — accepted
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// #1161: member access on a nullable INDEX result (`coll[i].member`)
// ---------------------------------------------------------------------------
// The §7.4 gate had branches for an identifier target and a member-access
// target, but none for an index/indexer target — so direct member access on a
// nullable index result slipped through and null-derefed at runtime. An index
// result is not a local variable, so it is never narrowable; it must be
// narrowed with `!`, `?.`, or a copy-to-local-then-guard.

constexpr const char* kCell =
    "module M; "
    "class Cell "
    "  public v: int; "
    "  public constructor(x: int) this.v := x; end constructor; "
    "end Cell; ";

// §7.4 (#1161): direct `.member` on an `OwnedBuffer<classT>` index result
// (typed `Cell?` per #1157) is rejected — narrow to non-null first.
BEZEL_TEST(nullable_field_narrowing_862, ownedbuffer_index_member_rejected_1161) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Cell>(2); "
        "  return buf[0].v; "  // §7.4: index result is Cell? — not narrowable
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldNarrowingError(r));
}

// §7.4 (#1161): a `Dictionary<K, V?>` indexer returns `V?`; direct `.member`
// on `d[k]` is likewise rejected.
BEZEL_TEST(nullable_field_narrowing_862, dict_nullable_value_index_member_rejected_1161) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Collections.Generic; "
        "class Cell "
        "  public v: int; "
        "  public constructor(x: int) this.v := x; end constructor; "
        "end Cell; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, Cell?>(); "
        "  return d[0].v; "  // §7.4: d[0] is Cell? — not narrowable
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldNarrowingError(r));
}

// §7.4 (#1161, force-unwrap): `buf[0]!.member` narrows the index result to
// non-null — it must be ACCEPTED.
BEZEL_TEST(nullable_field_narrowing_862, ownedbuffer_index_bang_member_accepted_1161) {
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Cell>(2); "
        "  buf[0] := new Cell(7); "
        "  return buf[0]!.v; "  // §7.4: `!` narrows — accepted
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#1161, no over-reject): a NON-nullable index result must still admit a
// bare `.member`. A record-element collection is value-typed, so its indexer is
// non-nullable — `buf[0].field` is fine and must NOT be flagged.
BEZEL_TEST(nullable_field_narrowing_862, nonnullable_record_index_member_accepted_1161) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Pt>(2); "
        "  buf[0] := new Pt(3, 4); "
        "  return buf[0].x; "  // §7.4: record elem is non-nullable — accepted
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
