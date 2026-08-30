// fix #3597 (§22.15 "Pure members" / §22.16 "Contract clauses" / §11.6
// "Nested types", v0.286): `checkPureBodies` (src/sema/PureChecker.cpp)
// carried the same one-level nested-type walk issue 3574 records for
// src/sema/TypeResolution_Checks.cpp -- but shallower along a second, DIFFERENT
// axis:
//
//   axis 1 -- §22.15's pure-body rule (the member-body walk) stopped at
//             nesting depth 2: a type nested TWO levels deep never had its
//             members' bodies walked at all.
//   axis 2 -- §22.15's pure-CONSTRUCTOR field-initializer rule (the
//             "birth-store" walk over a class's field initializers) ran
//             only for the OUTER type the top-level loop iterated -- it was
//             never applied to a nested type's own field initializers, so
//             it escaped already at nesting depth 1.
//
// Both axes are now closed by routing `checkPureBodies`'s per-type work
// (invariants + member-body walk + pure-ctor field-initializer walk, unified
// into `checkOneType` in PureChecker.cpp) through the shared recursive
// nested-type walker (fix #3574's `forEachTypeAndNested`, promoted to
// sema/NestedTypeWalk.hpp so this different translation unit can share it
// rather than carrying a sixteenth hand-copy of the walk).
//
// Measured on d8e07d7 (the #3574 base this fix builds on, which deliberately
// left this walk alone and filed the issue this fix closes):
//   axis 1: depth 0 -> 1 diagnostic, depth 1 -> 1, depth 2 -> 0 (bug)
//   axis 2: depth 0 -> 1 diagnostic, depth 1 -> 0 (bug)
//
// Each row below is a LADDER over depths 0..3, matching
// nested_type_walk_depth_3574_test.cpp's model, so the test pins the WALK.

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(const std::string& src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        bezel::sema::checkPureBodies(*r.sourceFile, *r.ctx);
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

// `inner` wrapped in `depth` levels of enclosing classes. depth 0 puts it at
// module level.
std::string atDepth(int depth, const std::string& inner,
                    const std::string& prelude = "") {
    std::string open, close;
    for (int i = 0; i < depth; ++i) {
        open += "public class W" + std::to_string(i) + " ";
        close = "end W" + std::to_string(i) + "; " + close;
    }
    return "module M; " + prelude + open + inner + " " + close;
}

// axis 1: a pure method body that assigns to a field -- §22.15's frame-local
// mutation root rule rejects a store outside the frame.
const char* kPureBodyFieldAssign =
    "public class Probe "
    "  public _x: int; "
    "  public pure method M(): int "
    "    _x := 1; "
    "    return _x; "
    "  end M; "
    "end Probe; ";

// axis 2: a pure constructor whose class has a field initializer that calls
// a non-pure method -- the birth-store rule.
const char* kPureCtorFieldInit =
    "public class Probe "
    "  public _y: int := Helper.Impure(); "
    "  public pure constructor() "
    "  end constructor; "
    "end Probe; ";
const char* kHelperPrelude =
    "class Helper "
    "  public static method Impure(): int "
    "    Console.WriteLine(\"side effect\"); "
    "    return 1; "
    "  end Impure; "
    "end Helper; ";

} // namespace

// Axis 1, #3597: §22.15's pure-body rule fires at every depth. Before the
// fix, depth 2 (and beyond) reported 0 -- the member-body walk never
// reached a type nested two levels deep.
BEZEL_TEST(sema_pure_checker_nested_3597,
           pure_body_rule_22_15_fires_at_every_depth) {
    for (int depth = 0; depth <= 3; ++depth) {
        auto r = parseSemaAll(atDepth(depth, kPureBodyFieldAssign));
        BEZEL_EXPECT_EQ(
            countDiags(r, "assignment is not admitted in a pure body"), 1);
    }
}

// Axis 2, #3597: the pure-constructor field-initializer (birth-store) rule
// fires at every depth. Before the fix, depth 1 (and beyond) reported 0 --
// the walk ran only for the type the old top-level loop iterated directly,
// never for a nested type.
BEZEL_TEST(sema_pure_checker_nested_3597,
           pure_ctor_field_initializer_rule_fires_at_every_depth) {
    for (int depth = 0; depth <= 3; ++depth) {
        auto r = parseSemaAll(
            atDepth(depth, kPureCtorFieldInit, kHelperPrelude));
        BEZEL_EXPECT_EQ(
            countDiags(r, "call is not admitted in a pure body"), 1);
    }
}

// The false-reject guard (§11.6): a LEGAL pure method and a legal pure
// constructor nested three levels deep must produce no diagnostic at all.
BEZEL_TEST(sema_pure_checker_nested_3597,
           legal_pure_members_nested_three_deep_not_newly_rejected) {
    auto r = parseSemaAll(atDepth(
        3,
        "public class Probe "
        "  public _z: int := 1; "
        "  public pure method M(): int "
        "    var v: int := 1; "
        "    return v; "
        "  end M; "
        "  public pure constructor() "
        "  end constructor; "
        "end Probe; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
