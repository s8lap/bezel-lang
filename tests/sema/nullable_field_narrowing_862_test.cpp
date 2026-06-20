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
