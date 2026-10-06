// Regression test for issue #5361 — spurious §28.3 auto-drop warning on a
// pre-declared out-owned recipient of OwningSet<T>.TryPop.
//
// The fix removes the builtinOutOwnedArg table entry for OwningSet.TryPop
// (arg 1). This entry was redundant: the formal parameter is declared
// `out owned`, so `userOutOwned` in markOutOwnedArgsCore (which checks the
// actual formal modifier) already classifies the argument correctly. The
// redundancy caused a spurious §28.3 auto-drop warning on the pre-declared
// out-owned recipient.
//
// Pre-fix: the recipient is marked Owned via builtinOutOwnedArg (in addition
// to userOutOwned), added to preDeclaredOutOwnedRecipients_, and the
// end-of-scope auto-drop check warns — spurious, because the transfer is
// already handled by the formal's out-owned classification.
// Post-fix: userOutOwned alone handles it correctly, no spurious warning.
//
// Derived from: stdlib §4.10 (OwningSet.TryPop signature, v0.259) and
// §28.3 (auto-drop warning), fix #5361.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

namespace {

// Builds a program that calls OwningSet<R>.TryPop with a pre-declared
// `var item: R? := null` as the out-owned recipient, followed by a trivial
// `return 0`. The query is s.First() so equality is guaranteed.
std::string makeProgram(const std::string& methodCall) {
    return std::string(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; _n := _n + 1; end constructor; "
        "  destructor _n := _n - 1; end destructor; "
        "  public override method Equals(other: Object?): bool "
        "    if other is R then var r := other as R; return r!.Tag = Tag; end; "
        "    return false; "
        "  end Equals; "
        "  public override method GetHashCode(): int => Tag; "
        "end R; "
        "class C public static method Main(): int "
        "  using var s := new OwningSet<R>(); "
        "  s.Add(new R(1)); s.Add(new R(2)); "
        "  var item: R? := null; "
        "  var q := s.First(); "
        + methodCall +
        "  return 0; "
        "end Main; end C;");
}

}  // namespace

// ---------------------------------------------------------------------------
// §28.3 (fix #5361): a pre-declared out-owned recipient of OwningSet.TryPop
// must NOT trigger the auto-drop warning. The formal parameter is `out owned`,
// so userOutOwned in markOutOwnedArgsCore handles the transfer correctly;
// builtinOutOwnedArg was redundant and caused a spurious warning.
// stdlib §4.10 (v0.259): TryPop(query: T, out owned removed: T?): bool
// ---------------------------------------------------------------------------

BEZEL_TEST(owning_set_trypop_autodrop_5361,
           trypop_predeclared_out_owned_no_spurious_autodrop_spec_28_3_4_10) {
    // OwningSet.TryPop(query, out owned removed): the second arg (position 1)
    // is the out-owned recipient. With the fix, no auto-drop warning fires for
    // the pre-declared `item` variable.
    auto r = parseAndCheckOwnership(
        makeProgram("var ok := s.TryPop(q, out item);"));
    // The program must compile cleanly (no hard errors).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The fix: no spurious §28.3 auto-drop warning on the pre-declared recipient.
    // Pre-fix: builtinOutOwnedArg redundantly added `item` to
    // preDeclaredOutOwnedRecipients_, firing the auto-drop check at scope end.
    // Post-fix: userOutOwned alone correctly handles the formal; no warning.
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "item"));
}
