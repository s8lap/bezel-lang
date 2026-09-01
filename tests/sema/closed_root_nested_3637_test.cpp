// fix #3637 (§19.6 "Exhaustiveness" / §11.7 "Static classes" / §11.6 "Nested
// types", v0.286): `Resolver::checkClosedHierarchyLeafVisibility`
// (src/sema/TypeResolution_Checks.cpp) collects the public `closed` roots it
// checks against via `ctx_.module()->children` -- TOP-LEVEL ONLY. A nested
// `public closed` class is legal (§11.6 "Nested types": a class body may
// declare nested classes) but is never added to `closedRoots`, so the WHOLE
// §19.6 leaf-visibility check below runs against an empty root set and
// produces no diagnostic, letting a concrete `internal` leaf of a public
// nested `closed` root escape the guard.
//
// This is the SAME defect shape as #3599 but on the root-collection side
// (which fix #3599 -- commit cbb7be5 / 3d309e5 -- explicitly left out, see
// that commit body's closing sentence: "the closed-ROOT collection ... stays
// restricted to top-level public classes -- that is a separate, narrower
// question from the leaf side this issue is about, and is flagged as issue
// 3599's follow-up rather than folded in here"). #3637 closes that follow-up.
//
// Cross-module SOUNDNESS hole, not merely a permissiveness gap: an importer
// proves `case` exhaustiveness over the nested `closed` root against an
// empty leaf set, while the runtime can still dispatch into the escaped leaf
// (reachable through the declaring module's own factory, §2.8.3 rule 4),
// raising `MatchFailureException` (§21.7) where the static proof said it
// could not.
//
// The fix routes the root collection through the same shared recursive
// nested-type walker (fix #3574, `Resolver::forEachTypeAndNested`) that fix
// #3599 used for the LEAF collection, filtering to
// `kind == Class && typeIsClosed && visibility == Public` inside the
// callback. The walker descends through nested types of every kind, so
// only the COLLECTED SET changes, not the descent.
//
// Measured on ee4f935 (the pre-fix base this fix builds on): the issue's
// nested repro exits 0 with no diagnostic at all (only the unrelated
// "no Main" complaint). Post-fix the same input draws the §19.6 diagnostic
// the top-level twin always drew -- the walk now finds `NestedRoot`.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"

#include <string>

namespace {

bezel::test::SemaResult resolveSrc(const std::string& src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

int countDiags(const bezel::test::SemaResult& r, const char* needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(needle) != std::string::npos) ++n;
    }
    return n;
}

} // namespace

// #3637: a concrete `internal` leaf of a NESTED public `closed` root draws
// the §19.6 diagnostic. Pre-fix this exits 0 silently because the root is
// never collected -- the WHOLE check runs against an empty root set.
BEZEL_TEST(sema_closed_root_nested_3637,
           nested_closed_root_internal_leaf_rejected) {
    auto r = resolveSrc(
        "module M; "
        "public class Outer "
        "  public closed class NestedRoot end NestedRoot; "
        "  internal class Leaf: Outer.NestedRoot end Leaf; "
        "end Outer; ");
    BEZEL_EXPECT_EQ(
        countDiags(r, "is a concrete subclass of `closed` class `NestedRoot`"),
        1);
}

// #3637 deeper: the closed root is two levels deep (nested inside a public
// class nested inside a public class). Pre-fix this is also silently
// accepted; post-fix the leaf is rejected just like a one-level-nested
// case. Pins that the walker descends through MULTIPLE non-root enclosing
// classes, not just one.
BEZEL_TEST(sema_closed_root_nested_3637,
           deeply_nested_closed_root_internal_leaf_rejected) {
    auto r = resolveSrc(
        "module M; "
        "public class Outer "
        "  public class Mid "
        "    public closed class DeepRoot end DeepRoot; "
        "    internal class Leaf: Mid.DeepRoot end Leaf; "
        "  end Mid; "
        "end Outer; ");
    BEZEL_EXPECT_EQ(
        countDiags(r, "is a concrete subclass of `closed` class `DeepRoot`"),
        1);
}

// #3637 non-regression control (the path #3599 was covering): a top-level
// public `closed` root with an internal leaf draws the §19.6 diagnostic,
// exactly as it did before #3637 -- this pins that the fix did not disturb
// the working top-level case.
BEZEL_TEST(sema_closed_root_nested_3637,
           toplevel_closed_root_still_rejected) {
    auto r = resolveSrc(
        "module M; "
        "public closed class TopRoot end TopRoot; "
        "internal class Leaf: TopRoot end Leaf; ");
    BEZEL_EXPECT_EQ(
        countDiags(r, "is a concrete subclass of `closed` class `TopRoot`"),
        1);
}

// #3637 false-reject guard: a NESTED `public closed` root with a PUBLIC
// concrete leaf satisfies §19.6 and must draw no diagnostic. The fix
// widens what is COLLECTED as a candidate root, not what is REJECTED -- a
// properly-public leaf stays clean.
BEZEL_TEST(sema_closed_root_nested_3637,
           nested_closed_root_public_leaf_not_rejected) {
    auto r = resolveSrc(
        "module M; "
        "public class Outer "
        "  public closed class Root end Root; "
        "  public class Leaf: Outer.Root end Leaf; "
        "end Outer; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #3637 narrow: an `internal` `closed` root (declared `internal`, even if
// nested inside a public class) has effective `internal` visibility and
// cannot be named cross-module (§13.2) -- the cross-module guarantee §19.6
// depends on does not apply. The new check must not flag its leaves even
// when those leaves are themselves `internal`. Pre-fix this was true
// trivially (the root was never collected); post-fix it stays true because
// the visibility filter still excludes an `internal` root.
BEZEL_TEST(sema_closed_root_nested_3637,
           internal_closed_root_leaves_unflagged) {
    auto r = resolveSrc(
        "module M; "
        "public class Outer "
        "  internal closed class InternRoot end InternRoot; "
        "  internal class Leaf: Outer.InternRoot end Leaf; "
        "end Outer; ");
    BEZEL_EXPECT_EQ(
        countDiags(r, "is a concrete subclass of `closed` class"), 0);
}
