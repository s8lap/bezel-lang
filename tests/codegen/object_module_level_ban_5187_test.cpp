// Regression tests for issue #5187 (spec §8.1, v0.311, unchanged at
// v0.313).
//
// AUTHORITY:
//
//   §8.1 *Definition* — "`Object` is the universal root." The ban, in
//   the same section's own words: "User code cannot redeclare or
//   replace `Object`. The ban is on a user module-level type declaration
//   — a class, record, enum, interface, choice or type alias — named
//   `Object` with no type parameters, in any user module and at any
//   visibility." The ban scope is therefore the bare module-level,
//   zero-arity `Object`. §8.1's next sentence permits every other
//   declaration of the name: "a generic `Object<T>` is a different name
//   (§2.2); a nested type or a type parameter named `Object` shadows the
//   root under §2.7 …" — so a generic instantiation, a nested `Object`
//   (§11.6) and a type-parameter named `Object` all remain ordinary
//   declarations.
//
//   §27.8 *Type aliases* — "Type aliases may be declared at module
//   level. A module-level alias is always **module-internal**", so a
//   module-level `type Object := int;` is a module-level type
//   declaration and falls under §8.1's ban, which names "type alias"
//   among its six banned declaration kinds.
//
// The diagnostic message includes the word `redeclare` to keep the §8.1
// corpus case's (`s8_1_0001`, baseline.tsv:256) needle functional when
// the banked divergence is retired.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// runAot + diagnostic-message-contains. Returns true iff the program is
// rejected at compile time AND one of the diagnostics carries `needle` as
// a substring. Mirrors the helper used by the other `*_rejected_*` tests
// in this directory.
bool diagContains(const std::string& prog, const char* needle) {
    auto r = runAot(prog);
    if (r.ran) return false;
    return diagnosticMessageContains(r, needle);
}

}  // namespace

// §8.1: a module-level `public class Object` is rejected. The `public`
// visibility is irrelevant to the ban — `class Object` (default
// visibility) is rejected identically — but the test uses `public` to
// mirror the spec-declared "user code cannot redeclare or replace
// `Object`" wording, which is most often invoked by library authors
// trying to expose a shadowed name.
BEZEL_TEST(object_module_level_ban_5187, public_class_object_rejected) {
    BEZEL_EXPECT_TRUE(diagContains(
        "module M; "
        "public class Object end Object; "
        "class C public static method Main(): int "
        "  return 0; "
        "end Main; end C;",
        "redeclare"));
}

// §27.8 + §8.1: a module-level type alias named `Object` is also
// rejected. §8.1's ban names "type alias" among the declaration kinds
// it covers, and §27.8 makes a module-level alias a module-level
// declaration, so the ban applies to the alias form identically.
BEZEL_TEST(object_module_level_ban_5187, type_alias_object_rejected) {
    BEZEL_EXPECT_TRUE(diagContains(
        "module M; "
        "type Object := int; "
        "class C public static method Main(): int "
        "  return 0; "
        "end Main; end C;",
        "redeclare"));
}