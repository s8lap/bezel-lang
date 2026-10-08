// Spec-derived regression test for issue #5630 — §9.7 / §28.1 record
// borrow-lifetime rule applied at generic instantiation.
//
// The rule: a borrow-bearing record may be returned only when every borrow it
// carries has a source the caller re-establishes. When a generic method
// `Pass<T>(owned a: T): owned T => a` is instantiated at a borrow-bearing
// record type (a record with at least one borrow-carrying field), the return
// value carries the borrow of the owned parameter `a`.
//
// When `Pass<Pair>(new Pair(x))` is called where `Pair(Item: Item)` is a
// borrow-bearing record and `x` is a scope-bound local, the returned Pair
// carries a borrow of `x`. Returning it from the method makes it outlive
// `x` (disposed at scope end), so §9.7 applies and the call must be rejected.
//
// The gap: carriedBorrowSourceOfValueExpr's CallExpr arm only checked
// identifier-root args; a NewExpr arg like `new Pair(x)` was skipped because
// identifierRoot(NewExpr) returns nullptr. Added recursive tracing for non-
// identifier args to find the underlying local source.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// Returns true if any diagnostic cites §9.7 and the record borrow-lifetime rule.
bool rejectsRecordBorrowEscapeOnReturn(const OwnershipResult& r,
                                      std::string_view sourceName) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§9.7") == std::string::npos) continue;
        if (d.message.find("record borrow-lifetime rule") == std::string::npos)
            continue;
        if (d.message.find(std::string(sourceName)) == std::string::npos)
            continue;
        return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// NEGATIVE — a borrow-bearing record returned via a generic call is rejected.
// =============================================================================

// The exact shape from the issue: Pass<T>(owned a: T): owned T => a instantiated
// at T=Pair (a borrow-bearing record). The returned Pair carries a borrow of the
// local `x` which is disposed at scope end.
BEZEL_TEST(return_borrow_generic_call_5630,
           pass_pair_via_owned_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module Test5630; "
        "public class Item "
        "  public constructor() end constructor; "
        "end Item; "
        "public record Pair(Item: Item); "
        "public class C "
        "  public static method Pass<T>(owned a: T): owned T => a; "
        "  public method M(): Pair "
        "    var x := new Item(); "
        "    return Pass<Pair>(new Pair(x)); "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsRecordBorrowEscapeOnReturn(r, "x"));
}

// The call argument is a bare NewExpr (no extra wrapper).
BEZEL_TEST(return_borrow_generic_call_5630,
           pass_pair_bare_new_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Item public constructor() end constructor; end Item; "
        "public record Pair(Item: Item); "
        "public class C "
        "  public static method Id<T>(owned a: T): owned T => a; "
        "  public method Run(): Pair "
        "    var x := new Item(); "
        "    return Id<Pair>(new Pair(x)); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsRecordBorrowEscapeOnReturn(r, "x"));
}

// =============================================================================
// POSITIVE — no over-rejection.
// =============================================================================

// A non-borrow-bearing record (value type) returned via the same generic call
// is admitted.
BEZEL_TEST(return_borrow_generic_call_5630,
           value_record_pass_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Val(I: int); "
        "public class C "
        "  public static method Id<T>(owned a: T): owned T => a; "
        "  public method Run(): Val "
        "    return Id<Val>(new Val(42)); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
