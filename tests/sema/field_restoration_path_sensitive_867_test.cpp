// fix #867 / §28.8.1: the field-restoration check must be PATH-SENSITIVE.
//
// A `move` of an owned field followed by a restoration on only SOME
// control-flow paths was wrongly ACCEPTED — the field was left moved-from
// (null) on the other path at method exit, violating §28.8.1 ("the field must
// be restored on EVERY path from the move to method exit"). The fix lifts the
// field-moved-from facts into the per-block dataflow state and merges them at
// CFG joins with OR-over-predecessors (moved-from on ANY incoming path →
// moved-from at the join), so a partial restore is now reported at exit.
//
// These tests pin the behavior on both the bare (`move _f`) and qualified
// (`move this._f`) forms (#825 form-parity), nullable fields, and loops, and
// guard against over-rejecting a valid all-paths restore.
// // per §28.8.1, #867

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Wrapper: a class with a non-nullable owned Logfile field, a no-op
// constructor, and a no-op destructor. Individual tests inline the method
// under scrutiny.
std::string withField(const std::string& method) {
    return std::string(
        "module M; "
        "class Logfile "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Logfile; "
        "class Holder "
        "  private owned _log: Logfile; "
        "  public constructor() _log := new Logfile(); end constructor; ") +
        method +
        " destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;";
}

// Wrapper with a NULLABLE owned field (null restores it per §28.8.1 / #825).
std::string withNullableField(const std::string& method) {
    return std::string(
        "module M; "
        "class Logfile "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Logfile; "
        "class Holder "
        "  private owned _log: Logfile?; "
        "  public constructor() _log := new Logfile(); end constructor; ") +
        method +
        " destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;";
}

// True iff some emitted diagnostic mentions the §28.8.1 field-restoration rule.
bool hasFieldRestorationError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("left in moved-from state") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §28.8.1 / #867: bare `move _log` restored only on the `c` path is left
// moved-from on the `c == false` path at method exit — must be REJECTED.
// This is the headline bug: before the fix it was silently accepted because
// the moved-from fact was not merged at the if-join.
BEZEL_TEST(field_restoration_path_sensitive_867, conditional_partial_restore_bare_rejected) {
    auto r = parseAndCheckOwnership(withField(
        "public method Maybe(c: bool): owned Logfile "
        "  var d := move _log; "
        "  if c then _log := new Logfile(); end; "
        "  return d; "
        "end Maybe;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldRestorationError(r));
}

// §28.8.1 / #867 (form-parity with #825): the qualified `move this._log` form
// with a conditional-partial restore must reject identically.
BEZEL_TEST(field_restoration_path_sensitive_867, conditional_partial_restore_qualified_rejected) {
    auto r = parseAndCheckOwnership(withField(
        "public method Maybe(c: bool): owned Logfile "
        "  var d := move this._log; "
        "  if c then this._log := new Logfile(); end; "
        "  return d; "
        "end Maybe;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldRestorationError(r));
}

// §28.8.1 / #867 (over-reject guard): both if/else branches restore — the
// field is restored on EVERY path, so this must be ACCEPTED. A wrong merge
// (meet instead of OR / dropping the then-branch's restore) would wrongly
// reject this.
BEZEL_TEST(field_restoration_path_sensitive_867, if_else_both_restore_accepted) {
    auto r = parseAndCheckOwnership(withField(
        "public method Swap(c: bool): owned Logfile "
        "  var d := move _log; "
        "  if c then _log := new Logfile(); "
        "  else _log := new Logfile(); end; "
        "  return d; "
        "end Swap;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 / #867 (over-reject guard): the restore happens BEFORE the branch,
// so the field is restored on every path regardless of the condition — must
// be ACCEPTED.
BEZEL_TEST(field_restoration_path_sensitive_867, restore_before_branch_accepted) {
    auto r = parseAndCheckOwnership(withField(
        "public method Swap(c: bool): owned Logfile "
        "  var d := move _log; "
        "  _log := new Logfile(); "
        "  if c then end; "
        "  return d; "
        "end Swap;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 / #867: nullable owned field, conditional-partial `null` restore —
// the field is left moved-from on the `c == false` path; must be REJECTED.
BEZEL_TEST(field_restoration_path_sensitive_867, nullable_conditional_partial_rejected) {
    auto r = parseAndCheckOwnership(withNullableField(
        "public method Maybe(c: bool): owned Logfile? "
        "  var d := move _log; "
        "  if c then _log := null; end; "
        "  return d; "
        "end Maybe;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldRestorationError(r));
}

// §28.8.1 / #867 (over-reject guard): nullable owned field restored with
// `null` on ALL paths — `null` restores a nullable owned field (§28.8.1 /
// #825), so this must be ACCEPTED.
BEZEL_TEST(field_restoration_path_sensitive_867, nullable_all_paths_null_restore_accepted) {
    auto r = parseAndCheckOwnership(withNullableField(
        "public method Detach(c: bool): owned Logfile? "
        "  var d := move _log; "
        "  if c then _log := null; else _log := null; end; "
        "  return d; "
        "end Detach;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 / #867: a `move` whose restore is inside a conditionally-entered
// loop body is NOT restored on the zero-trip path (loop never runs) — must be
// REJECTED. The loop back-edge / zero-trip skip meet must carry the moved-from
// fact through.
BEZEL_TEST(field_restoration_path_sensitive_867, loop_partial_restore_rejected) {
    auto r = parseAndCheckOwnership(withField(
        "public method M(n: int): owned Logfile "
        "  var d := move _log; "
        "  var i := 0; "
        "  while i < n do "
        "    if i = 5 then _log := new Logfile(); end; "
        "    i := i + 1; "
        "  end; "
        "  return d; "
        "end M;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldRestorationError(r));
}

// §28.8.1 / #867 (over-reject guard): the restore happens before a loop whose
// body never touches the field — the field is restored on every path,
// including the zero-trip path; must be ACCEPTED.
BEZEL_TEST(field_restoration_path_sensitive_867, loop_full_restore_accepted) {
    auto r = parseAndCheckOwnership(withField(
        "public method M(n: int): owned Logfile "
        "  var d := move _log; "
        "  _log := new Logfile(); "
        "  var i := 0; "
        "  while i < n do "
        "    i := i + 1; "
        "  end; "
        "  return d; "
        "end M;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
