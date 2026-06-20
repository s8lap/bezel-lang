// fix #830 / §28.10: "`using var` is implicitly non-reassignable. Reassigning a
// `using var` variable is a compile error." Before the fix the reassignment was
// accepted silently — only the SECOND resource was disposed at scope end and the
// first leaked. These tests verify the reject fires for a plain owned-class
// `using var` and for a `Shared<T>` `using var`, and that legitimate forms
// (correct `using var`, plain-`var` reassignment, the `do…end` idiom, the `_`
// discard) are NOT rejected.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class R public static n: int := 0; "
        "  destructor R.n := R.n + 1; end destructor; end R; ") + body;
}

// True iff some emitted diagnostic message mentions the §28.10 reassignment rule.
bool hasUsingVarReassignError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("using var") != std::string::npos &&
            d.message.find("reassign") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §28.10: reassigning a plain owned-class `using var` local is a compile error.
BEZEL_TEST(using_var_reassign_830, plain_owned_reassign_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class C public static method Fill() "
        "  using var x := new R(); "
        "  x := new R(); "
        "end Fill; end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUsingVarReassignError(r));
}

// §28.10: reassigning a `Shared<T>` `using var` is likewise rejected.
BEZEL_TEST(using_var_reassign_830, shared_reassign_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public static n: int := 0; "
        "  destructor Inner.n := Inner.n + 1; end destructor; end Inner; "
        "class C public static method Fill() "
        "  var i1 := new Inner(); "
        "  using var h1 := new Shared<Inner>(i1); "
        "  var i2 := new Inner(); "
        "  using var h2 := new Shared<Inner>(i2); "
        "  h1 := h2.Clone(); "
        "end Fill; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUsingVarReassignError(r));
}

// A correctly-used `using var` (never reassigned) is accepted.
BEZEL_TEST(using_var_reassign_830, correct_using_var_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public static method Fill() "
        "  using var x := new R(); "
        "end Fill; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The §28.10-recommended idiom — a fresh `do…end` block per resource — is fine.
BEZEL_TEST(using_var_reassign_830, do_block_fresh_resource_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public static method Fill() "
        "  do using var x := new R(); end; "
        "  do using var y := new R(); end; "
        "end Fill; end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A plain `var` (not `using var`) value-type local may still be reassigned.
BEZEL_TEST(using_var_reassign_830, plain_var_reassign_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class C public static method Main(): int "
        "  var x := 3; x := 7; return x; "
        "end Main; end C;"));
    BEZEL_EXPECT_FALSE(hasUsingVarReassignError(r));
}

// The `_` discard (`_ := expr`) is not a `using var` rebind and must not be
// flagged by the §28.10 rule.
BEZEL_TEST(using_var_reassign_830, discard_assignment_not_flagged) {
    auto r = parseAndCheckOwnership(mod(
        "class C public static method Main(): int "
        "  _ := 99; return 5; "
        "end Main; end C;"));
    BEZEL_EXPECT_FALSE(hasUsingVarReassignError(r));
}
