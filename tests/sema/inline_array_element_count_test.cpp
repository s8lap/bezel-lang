// Spec-derived sema tests (sweep #11) — an inline-array literal initializer
// must contain EXACTLY N elements.
//
// Authority: §15.4 — "The list must contain exactly N elements; mismatch is a
// compile error." §15.1 line 1940 likewise. Before the fix, too few elements
// left uninitialized slots (garbage reads) and too many overran the slot
// storage at codegen (SIGSEGV) — both compiled silently.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasElementCountError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("inline-array initializer has") != std::string::npos &&
            d.message.find("exactly") != std::string::npos)
            return true;
    return false;
}

std::string wrap(const std::string& body) {
    return std::string("module M; class C public static method Main(): int ") +
           body + " end Main; end C;";
}

}  // namespace

// §15.4: too FEW elements is a compile error (was: garbage reads).
BEZEL_TEST(inline_array_element_count, too_few_elements_rejected) {
    auto r = parseSemaAll(wrap("var a: [5]int := [1, 2, 3]; return a[0];"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasElementCountError(r));
}

// §15.4: too MANY elements is a compile error (was: runtime SIGSEGV).
BEZEL_TEST(inline_array_element_count, too_many_elements_rejected) {
    auto r = parseSemaAll(wrap("var a: [3]int := [1, 2, 3, 4, 5]; return a[0];"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasElementCountError(r));
}

// §15.4: exactly N elements is accepted.
BEZEL_TEST(inline_array_element_count, exact_elements_accepted) {
    auto r = parseSemaAll(wrap("var a: [3]int := [10, 20, 30]; return a[0];"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.1: a `const`-folded size N also enforces the exact-N rule.
BEZEL_TEST(inline_array_element_count, const_sized_exact_accepted) {
    auto r = parseSemaAll(
        "module M; class C public const N: int := 3; "
        "public static method Main(): int "
        "  var a: [N]int := [7, 8, 9]; return a[0] + a[1] + a[2]; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.1: a `const`-folded size with the wrong element count is rejected.
BEZEL_TEST(inline_array_element_count, const_sized_mismatch_rejected) {
    auto r = parseSemaAll(
        "module M; class C public const N: int := 4; "
        "public static method Main(): int "
        "  var a: [N]int := [7, 8, 9]; return a[0]; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasElementCountError(r));
}

// ===========================================================================
// §15.1 (#953): N must be greater than zero — `[0]T` is a compile error.
// ===========================================================================

// REJECT: a zero-sized inline array (no initializer).
BEZEL_TEST(inline_array_element_count, zero_size_rejected) {
    auto r = parseSemaAll(wrap("var a: [0]int; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("greater than zero") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// REJECT: a zero-sized inline array via a `const` size.
BEZEL_TEST(inline_array_element_count, zero_size_via_const_rejected) {
    auto r = parseSemaAll(
        "module M; class C public const Z: int := 0; "
        "public static method Main(): int var a: [Z]int; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ACCEPT: a positive-sized inline array is fine.
BEZEL_TEST(inline_array_element_count, positive_size_accepted) {
    auto r = parseSemaAll(wrap("var a: [3]int; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §15.1 (#953): an inline array may not be the element of another inline array
// (`[N][M]T`) — multi-dimensional inline arrays are not admitted.
BEZEL_TEST(inline_array_element_count, multi_dimensional_rejected) {
    auto r = parseSemaAll(wrap("var m: [2][3]int; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("element of another inline array") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// One-dimensional inline arrays remain accepted.
BEZEL_TEST(inline_array_element_count, one_dimensional_accepted) {
    auto r = parseSemaAll(wrap("var a: [3]int; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
