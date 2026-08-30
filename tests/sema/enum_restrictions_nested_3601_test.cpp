// fix #3601 (§10.4 "Methods, properties, static members" / §10.5
// "Restrictions" / §11.6 "Nested types", v0.286): `Resolver::checkEnumRestrictions`
// (src/sema/TypeResolution_Checks.cpp) used to enforce §10.5's restriction set
// over MODULE-LEVEL enums only, plus a one-level carve-out (fix #3044) that
// reached §10.4's `FromUnderlyingOrNull` reject through exactly one level of
// nesting. Both bounds were independently observable:
//
//   (1) §10.5's restrictions (instance fields, non-static constructors, nested
//       types, block-form/backing-storage properties, `event` members,
//       virtual/abstract/override methods) never reached a nested enum AT ALL
//       -- the escape started at depth 1.
//   (2) §10.4's `FromUnderlyingOrNull` carve-out (fix #3044) walked exactly one
//       level of nesting, so it reached a depth-1 nested enum but not one
//       nested two levels down.
//
// Both loops are now routed through the shared recursive nested-type walker
// (fix #3574, `Resolver::forEachTypeAndNested`), filtering to `kind == Enum`
// inside the callback, so a single walk reaches every enum at every depth with
// the FULL §10.5 restriction set (which already includes the
// `FromUnderlyingOrNull` carve-out) -- closing both axes together and
// retiring the old separate module-children-only `FromUnderlyingOrNull` loop
// (kept alongside, it would double-report a module-level enum's violation).
//
// Measured on 907ce0f (pre-fix), reproduced on d8e07d7 (the #3574 base this
// fix builds on, which deliberately left this walk alone):
//
//   axis (1) §10.5 instance-field rule:     depth 0 -> 1 diagnostic
//                                            depth 1 -> 0 diagnostics (bug)
//   axis (2) §10.4 FromUnderlyingOrNull:     depth 1 -> 1 diagnostic
//                                            depth 2 -> 0 diagnostics (bug)
//
// Each row below is a LADDER over depths 0..3 (matching
// nested_type_walk_depth_3574_test.cpp's model) rather than a single deep
// case, so the test pins the WALK, not one caller.

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

// `inner` wrapped in `depth` levels of enclosing classes. depth 0 puts it at
// module level; depth 1 is the first shape axis (1) never reached.
std::string atDepth(int depth, const std::string& inner) {
    std::string open, close;
    for (int i = 0; i < depth; ++i) {
        open += "public class W" + std::to_string(i) + " ";
        close = "end W" + std::to_string(i) + "; " + close;
    }
    return "module M; " + open + inner + " " + close;
}

// §10.5: an enum declaring an instance field -- the first of the restriction
// set's rules, and representative of the whole list (the same walk gates all
// of them).
const char* kEnumInstanceField =
    "public enum G "
    "  A := 1; "
    "  public _x: int; "
    "end G; ";

// §10.4 (fix #3044): a user declaration of the compiler-provided
// `FromUnderlyingOrNull` static member.
const char* kEnumFromUnderlyingOrNull =
    "public enum E "
    "  A := 1; "
    "  public static method FromUnderlyingOrNull(v: int): int "
    "    return v; "
    "  end FromUnderlyingOrNull; "
    "end E; ";

} // namespace

// Axis (1), #3601: §10.5's instance-field restriction fires at every depth.
// Before the fix, depth 1 (and beyond) reported 0 -- the rule never reached a
// nested enum at all.
BEZEL_TEST(sema_enum_restrictions_nested_3601,
           instance_field_rule_10_5_fires_at_every_depth) {
    for (int depth = 0; depth <= 3; ++depth) {
        auto r = resolveSrc(atDepth(depth, kEnumInstanceField));
        BEZEL_EXPECT_EQ(
            countDiags(r, "may not declare an instance field"), 1);
    }
}

// Axis (2), #3601: §10.4's FromUnderlyingOrNull carve-out fires at every
// depth. Before the fix, depth 2 (and beyond) reported 0 -- the #3044
// carve-out walked exactly one level of nesting.
BEZEL_TEST(sema_enum_restrictions_nested_3601,
           from_underlying_or_null_rule_10_4_fires_at_every_depth) {
    for (int depth = 0; depth <= 3; ++depth) {
        auto r = resolveSrc(atDepth(depth, kEnumFromUnderlyingOrNull));
        BEZEL_EXPECT_EQ(
            countDiags(r,
                       "may not declare a member named `FromUnderlyingOrNull`"),
            1);
    }
}

// No-double-report guard, #3601: a MODULE-LEVEL enum's FromUnderlyingOrNull
// violation (depth 0) must be reported exactly once. The pre-fix compiler had
// two separate loops -- a module-children-only `FromUnderlyingOrNull` loop
// covering nested enums only, and the §10.5 loop covering module-level enums
// only -- so folding them into one walk without retiring the first would have
// been safe at depth 0 by construction (the first loop explicitly skipped
// enum `outer`s); this pins that the merged walk keeps that count at 1 rather
// than accidentally introducing a double visit.
BEZEL_TEST(sema_enum_restrictions_nested_3601,
           from_underlying_or_null_module_level_not_double_reported) {
    auto r = resolveSrc(atDepth(0, kEnumFromUnderlyingOrNull));
    BEZEL_EXPECT_EQ(
        countDiags(r, "may not declare a member named `FromUnderlyingOrNull`"),
        1);
}

// The false-reject guard (§11.6): a LEGAL enum nested three levels deep --
// declaring only what §10.4/§10.5 admit (a static field-free constant list,
// no instance state) -- must produce no diagnostic at all.
BEZEL_TEST(sema_enum_restrictions_nested_3601,
           legal_enum_nested_three_deep_is_not_newly_rejected) {
    auto r = resolveSrc(atDepth(
        3,
        "public enum Legal "
        "  A := 1, B := 2; "
        "  public static method Describe(v: Legal): string => \"legal\"; "
        "end Legal; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
