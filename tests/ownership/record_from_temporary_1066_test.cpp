// Spec-derived tests for #1066 / §9.7 (v0.153) + §28.3: a BORROW-BEARING record
// may not be constructed from a TEMPORARY bound to one of its borrow-carrying
// fields. The temporary's lifetime ends at the construction expression, so the
// record would immediately outlive that borrow's source (a dangling borrow). The
// repro `new Bag([10,20,30])` where `record Bag(Items: Array<int>)` borrows the
// array literal previously compiled and read a dangling/zero-length array.
//
// §9.7 (v0.153): "A borrow-bearing record may not be constructed from a temporary
// bound to one of its borrow-carrying fields … bind the source to a named local
// whose scope outlives the record, and pass that."
//
// A temporary is a non-place expression: a collection/array literal, a `new`, or
// an unbound call/operator result. A named local/parameter/field (a place) whose
// storage outlives the record stays ACCEPTED. A value-typed record (no borrow-
// carrying field) is never borrow-bearing, so constructing it from a temporary is
// fine.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// `Bag` is borrow-bearing (Array<int> is a class-typed borrow field, §9.7); `Pt`
// is a value-typed record. `Run` takes a borrow parameter `xs` for the place
// (long-lived-borrow) cases.
std::string mod(const std::string& body) {
    return std::string("module M; "
        "record Bag(Items: Array<int>); "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public method Run(xs: Array<int>) " + body + " end Run; "
        "end C;");
}

} // namespace

// ---- NEGATIVE: borrow-bearing record from a temporary → REJECT ----

// The #1066 repro: a local bound to `new Bag(<array literal>)`.
BEZEL_TEST(record_from_temporary_1066, local_decl_from_array_literal_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "var b := new Bag([10, 20, 30]);"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// An assignment from a temporary is the same dangling borrow.
BEZEL_TEST(record_from_temporary_1066, assignment_from_array_literal_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "var b := new Bag(xs); "   // place arg — accepted
        "b := new Bag([1, 2]);"));  // temporary arg — rejected
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---- POSITIVE: must stay ACCEPTED ----

// A named local whose scope outlives the record: `var xs2 := […]; new Bag(xs2);`.
// xs2 is a place; the borrow's source outlives the record. ACCEPT.
BEZEL_TEST(record_from_temporary_1066, long_lived_local_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "var xs2: Array<int> := [1, 2, 3]; "
        "var b := new Bag(xs2);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A borrow parameter (a place with caller-frame storage) is the back-reference
// construction form. ACCEPT.
BEZEL_TEST(record_from_temporary_1066, back_reference_param_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "var b := new Bag(xs);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A value-typed record (`Pt`, int fields) is not borrow-bearing — constructing it
// from literal args carries no borrow, so it is fine. ACCEPT.
BEZEL_TEST(record_from_temporary_1066, value_record_from_temp_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "var p := new Pt(1, 2);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
