// fix #4339 (§28.3, v0.298): a sibling (non-nested) SECOND `if`
// statement following a first `if` whose branches clash silently
// suppressed that clash's diagnostic. The CFG builder's `visitBranch`
// promotes the predecessor block to `Kind::Branch` in place; when the
// predecessor is a Merge block (a prior sibling `if`'s exit), the
// in-place kind overwrite made `isIfMerge` false on the final emit
// pass, the §28.3 branch-join clash report was suppressed, and the
// poisoned merge state silently leaked the rejected branch's value
// into codegen — measured as one Res per loop iteration under
// LeakSanitizer at 200x amplification (issue 4339's leak repro).
//
// These tests assert the diagnostic fires for every shape that
// previously hid it. None of these reach `ownedBorrowedClash`'s own
// `report` call on the parent commit `1c215ac`, even though every
// shape is a real branch-join mismatch the §28.3 spec names.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// Wrap a method body that uses `a`/`b`/`i` as scope-available names.
// Declares all of them as parameters so identifier-resolution does
// not emit unrelated diagnostics.
std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(a: bool): int "
        ) + body + " end Main; end C;";
}

} // namespace

// The original repro from issue 4339: a second sibling `if` that never
// touches `w` MUST NOT suppress the first `if`'s owned-vs-borrowed
// branch-join clash diagnostic. Pre-fix, the diagnostic silently
// disappeared (rc=0, zero relevant diagnostics) and the corrupted
// poisoned merge state leaked one Res per loop iteration under LSan
// (3200 bytes / 200 objects in the issue's 200x-amplified leak repro).
BEZEL_TEST(own_join, sibling_second_if_does_not_suppress_branch_ownership_clash) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(5); "
        "var w := x; "
        "if a then w := new Res(1); end; "
        "var q := 1; "
        "var t := 0; "
        "if a then var u := 1; end; "  // any second sibling if, even one
                                        // that never touches w
        "x.Dispose(); "
        "return t;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "branch ownership conflict"));
}

// A second sibling `if` that REASSIGNS `w` — the more obvious shape
// where the suppressor and the suppressee are both `w`-touching. The
// diagnostic MUST still fire on the FIRST `if`'s merge, not be lost
// when the second `if`'s setup also touches the same local. Pre-fix,
// both merges were silent for the same reason (Merge's kind
// overwritten by the next sibling's branch promotion); post-fix
// BOTH merges' clash reports fire independently.
BEZEL_TEST(own_join, sibling_second_if_touching_w_still_diagnoses_first_merge) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new Res(5); "
        "var w := x; "
        "if a then w := new Res(1); end; "  // FIRST merge: clash
        "var q := 1; "
        "if a then w := new Res(2); end; "  // SECOND merge: another clash
        "x.Dispose(); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "branch ownership conflict"));
}