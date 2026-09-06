// fix #3853 — retire the impl-ahead static `String.Compare` (stdlib §9.6,
// v0.290; proposal 0123 R6), and add the instance `CompareTo(other,
// ignoreCase)` beside `Equals(other, ignoreCase)`.
//
// WHAT v0.290 RETIRED. `String.Compare(a: string, b: string)` and its 3-arg
// `Compare(a, b, ignoreCase: bool)` overload were accepted (and lowered in
// codegen) by the compiler, but were never declared in stdlib §9.6's
// `String` member list at v0.290 or later — an impl-ahead-of-spec surface,
// same class as fix #3854's `IsNullOrEmpty`. Docs Bezel_Changelog.md's
// v0.290 entry (R6(f)) records the replacement explicitly: "the static
// `String.Compare(a, b[, ignoreCase])` becomes the instance `CompareTo(other,
// ignoreCase)` beside `Equals(other, ignoreCase)`". This file pins the
// compiler catch-up: the sema closed-set static-member arm
// (`ExprTypecheck_Calls.cpp`), the registry rows (`BuiltinRegistry.cpp`), and
// the codegen lowering (`LlvmBackend_Calls2.cpp`) all dropped the static
// surface; `tryLowerStringMethod` (`LlvmBackend_String.cpp`) gained the 2-arg
// instance overload.
//
// THE #2581/#2315 INTERACTION. The retired static form declared NON-nullable
// `a`/`b`, protected by a dedicated #2581 assignability check
// (ExprTypecheck_Calls.cpp, since deleted alongside the arm it protected).
// The new instance member's `other` parameter is likewise non-nullable, but
// it is now covered by the GENERAL #2315 primitive-string-instance gate
// (`checkBuiltinStringCallArgs`), which already listed `CompareTo` at
// position 0 before this fix (added when the 1-arg `CompareTo(other)` was
// implemented, fix #586) — so the 2-arg overload inherits the same
// protection with no new gate needed. See
// `tests/sema/builtin_span_nullable_arg_2581_test.cpp` for the migrated
// nullable-argument (and nullable-RECEIVER, a shape the static form could
// not express) regression rows.
//
// WHY THE DIAGNOSTIC TEXT MATTERS. `Compare` and a typo of some other member
// both produce "no such member" from the compiler's point of view, but only
// one of them is worth re-reading source for. The reject rows below assert
// the REMOVAL wording (naming the real replacement) rather than
// `hasErrors()`, which cannot distinguish this rule from an unrelated
// unknown-member error.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

JitResult reject(const std::string& src) { return runAot(src); }

// True iff some ERROR diagnostic is the v0.290 removal wording for
// `String.Compare`, naming the real replacement spelling.
bool removedStringCompare(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        const std::string& m = d.message;
        if (m.find("'String.Compare' was REMOVED from the standard "
                    "library at v0.290 (stdlib \xC2\xA7""9.6)") ==
            std::string::npos)
            continue;
        if (m.find("a.CompareTo(b)") == std::string::npos) continue;
        if (m.find("a.CompareTo(b, ignoreCase := true)") == std::string::npos)
            continue;
        if (m.find("it is gone, not misspelled (issue 3853)") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// REJECT — `string.Compare` (both arities), the `String` alias, and a bare
// method reference (§27.6's `(string, string) -> int` shape, e.g. a
// `Sorted` comparer).
// ===========================================================================

BEZEL_TEST(sema_string_compare_removed_3853, reject_two_arg_direct_call) {
    auto r = reject(prog("    return string.Compare(\"a\", \"b\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedStringCompare(r));
}

BEZEL_TEST(sema_string_compare_removed_3853, reject_three_arg_ignorecase_call) {
    auto r = reject(prog(
        "    return string.Compare(\"A\", \"a\", true);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedStringCompare(r));
}

BEZEL_TEST(sema_string_compare_removed_3853, reject_uppercase_string_alias) {
    // §3.4: `String` and `string` are interchangeable — the reject must not
    // depend on which spelling names the primitive.
    auto r = reject(prog("    return String.Compare(\"a\", \"b\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedStringCompare(r));
}

// §27.6 / stdlib §4.4: a primitive-intrinsic static method used as a bare
// method reference (not the direct callee of a call) is the OTHER branch
// `methodRef` used to serve for `Compare` — a `(string, string) -> int`
// comparer passed to `Array<string>.Sorted`. This must reject exactly like
// the direct-call shape.
BEZEL_TEST(sema_string_compare_removed_3853, reject_bare_method_reference) {
    auto r = reject(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var arr := new Array<string>();\n"
        "    arr.Add(\"b\"); arr.Add(\"a\");\n"
        "    using var sorted := arr.Sorted(string.Compare);\n"
        "    return sorted.Count;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(removedStringCompare(r));
}

// ===========================================================================
// ACCEPT-CONTROLS — the sibling static `IsNullOrWhiteSpace` (same
// `if (pt == PrimitiveType::String)` closed-set block) and the new instance
// `CompareTo` replacement both keep working.
// ===========================================================================

BEZEL_TEST(sema_string_compare_removed_3853,
           accept_isnullorwhitespace_sibling_still_works) {
    // Deleting the `Compare` arm from the shared static-methods block must
    // not disturb its neighbor.
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine((if "
                        "string.IsNullOrWhiteSpace(\"   \") then 1 else 0));\n"
                        "    return 0;")),
                    "1");
}

BEZEL_TEST(sema_string_compare_removed_3853,
           accept_instance_compareto_replacement_one_arg) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(\"a\".CompareTo(\"b\"));\n"
                        "    return 0;")),
                    "-1");
}

BEZEL_TEST(sema_string_compare_removed_3853,
           accept_instance_compareto_replacement_ignorecase) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    Console.WriteLine(\"ABC\".CompareTo(\"abc\", ignoreCase := "
            "true));\n"
            "    return 0;")),
        "0");
}

// NOT covered here: whether the bare RECEIVER-nullability trap #2581/#2315
// hands off to for `CompareTo`'s argument, or the general §7.4 gate for its
// RECEIVER, still holds. That is `tests/sema/builtin_span_nullable_arg_2581_
// test.cpp`'s `instance_compareto_*` rows, migrated in the same commit.
