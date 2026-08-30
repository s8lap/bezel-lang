// fix #3599 (§19.6 "Exhaustiveness" / §11.7 "Static classes" / §11.6 "Nested
// types", v0.286): `Resolver::checkClosedHierarchyLeafVisibility`
// (src/sema/TypeResolution_Checks.cpp) builds the leaf set of a `closed` root
// by walking every class declared in the module. Its `collectNested` lambda
// used to stop DESCENDING as soon as it met a nested type that was not a
// class -- so a concrete leaf class declared inside a nested RECORD (or any
// other non-class nested type) was silently excluded from the leaf set §19.6
// checks. §11.6 states "Nested types in records are permitted but uncommon",
// so a class CAN be declared inside a nested record; such a class used to
// escape the guard entirely.
//
// This is a cross-module soundness hole, not merely a permissiveness gap: an
// importer proves `case` exhaustiveness over the `closed` root against an
// INCOMPLETE leaf set (only public leaves ride the metadata into the
// importer's symbol table, and the check exists to require every concrete
// leaf be public), and the runtime can still dispatch into the escaped leaf
// (reachable through the declaring module's own factory, §2.8.3 rule 4),
// raising `MatchFailureException` (§21.7) where the static proof said it
// could not.
//
// The fix routes the leaf collection through the shared recursive nested-type
// walker (fix #3574, `Resolver::forEachTypeAndNested`), filtering to
// `kind == Class` inside the callback -- the walker descends through nested
// types of every kind, so only the collected SET changes, not the descent.
//
// Measured on d8e07d7 (the #3574 base this fix builds on, which deliberately
// left this walk alone and filed the issue this fix closes): a concrete
// `internal` leaf nested inside a nested CLASS is correctly rejected (1
// diagnostic); the same leaf nested inside a nested RECORD instead is
// silently accepted (0 diagnostics) -- exit 0, no diagnostic at all.
//
// The ladder below drives the class-nesting depth from 0..3 with one
// intervening RECORD layer inserted at the deepest point, so the test pins
// the WALK's descent through a non-class nested type rather than one single
// repro shape.

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

const char* kRootPrelude =
    "public closed class Root "
    "end Root; ";

// `Leaf` wrapped in `depth` levels of enclosing classes, with one further
// RECORD layer between the last class and `Leaf` -- the shape the pre-fix
// walk never descended through. depth 0 puts a record directly at module
// level (`public record R ... internal class Leaf ...`).
std::string leafUnderRecordAtDepth(int depth) {
    std::string open, close;
    for (int i = 0; i < depth; ++i) {
        open += "public class W" + std::to_string(i) + " ";
        close = "end W" + std::to_string(i) + "; " + close;
    }
    return std::string("module M; ") + kRootPrelude + open +
           "public record R(x: int) "
           "  internal class Leaf: Root "
           "  end Leaf; "
           "end R; " +
           close;
}

} // namespace

// #3599: a concrete `internal` leaf nested inside a nested RECORD is
// rejected at every class-nesting depth of the enclosing chain, matching the
// nested-CLASS case's behaviour (which already worked pre-fix).
BEZEL_TEST(sema_closed_leaf_nested_record_3599,
           leaf_under_nested_record_rejected_at_every_depth) {
    for (int depth = 0; depth <= 3; ++depth) {
        auto r = resolveSrc(leafUnderRecordAtDepth(depth));
        BEZEL_EXPECT_EQ(
            countDiags(r, "is a concrete subclass of `closed` class `Root`"),
            1);
    }
}

// Sibling shape, #3599 baseline: the same leaf nested inside a nested CLASS
// (not a record) was already rejected pre-fix -- this pins that the fix does
// not disturb that path.
BEZEL_TEST(sema_closed_leaf_nested_record_3599,
           leaf_under_nested_class_still_rejected) {
    auto r = resolveSrc(
        "module M; " + std::string(kRootPrelude) +
        "public class Outer "
        "  internal class Leaf: Root "
        "  end Leaf; "
        "end Outer; ");
    BEZEL_EXPECT_EQ(
        countDiags(r, "is a concrete subclass of `closed` class `Root`"), 1);
}

// False-reject guard, §11.6: a `public` leaf nested inside a nested record
// satisfies §19.6's requirement and must draw no diagnostic -- the fix widens
// what the walk COLLECTS, not what it REJECTS; a properly-exported leaf stays
// clean at every depth the ladder covers.
BEZEL_TEST(sema_closed_leaf_nested_record_3599,
           public_leaf_under_nested_record_not_rejected) {
    auto r = resolveSrc(
        "module M; " + std::string(kRootPrelude) +
        "public class Outer "
        "  public record R(x: int) "
        "    public class Leaf: Root "
        "    end Leaf; "
        "  end R; "
        "end Outer; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
