// Regression tests for issue #5187 (spec §8.1, v0.311).
//
// AUTHORITY:
//
//   §8.1 *Definition* — "`Object` is the universal base type … user code
//   cannot redeclare or replace `Object`." The ban scope is the bare
//   module-level, zero-arity `Object`: a generic `Object<T>` (a §11.6
//   generic instantiation shadows the root under §2.7), a nested
//   `Object` (a §13.2 nested type shadows the root under §2.7), and a
//   type-parameter named `Object` (a §11.8 type parameter shadows the
//   root under §2.7) all remain ordinary declarations per the issue's
//   scope rule.
//
//   §27.8 *Type aliases* — "A type alias is a top-level type
//   declaration," so the §8.1 ban applies identically to a module-level
//   `type Object := int;`.
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
// rejected (a type alias is a top-level type declaration, so the §8.1
// ban applies identically to the alias form).
BEZEL_TEST(object_module_level_ban_5187, type_alias_object_rejected) {
    BEZEL_EXPECT_TRUE(diagContains(
        "module M; "
        "type Object := int; "
        "class C public static method Main(): int "
        "  return 0; "
        "end Main; end C;",
        "redeclare"));
}