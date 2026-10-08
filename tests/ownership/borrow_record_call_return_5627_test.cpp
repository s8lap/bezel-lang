// Spec-derived regression test for issue #5627 — §9.7 borrow-lifetime
// bullet 1: a borrow-bearing record returned from a call may not outlive
// the source of any borrow it carries.
//
// §9.7 (v0.326) borrow-lifetime bullet 1: "A borrow-bearing record value
// may not outlive the source of any borrow it contains."
//
// The bug: carriedBorrowSourceOfValueExpr (Ownership_Checks.cpp) handled
// NewExpr (record construction with positional args) and bare IdentifierExpr
// (direct local reference), but NOT CallExpr returning a borrow-bearing
// inline array of records. Helper.Wrap(b) returning [1]Str2 was invisible to
// the outer-local escape check, so outer := Helper.Wrap(b) (outer at method
// scope, b inside a do block) compiled clean and UAF'd at runtime.
//
// Two arms:
//   (A) REJECT — source b in inner block, record in outer local
//   (B) ACCEPT — same program with b hoisted to method scope

#include <string>

#include "support/aot_support.hpp"        // runAotChecked — AOT + ASan/LSan
#include "support/ownership_support.hpp"  // parseAndCheckOwnership
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kRecordOutlivesNeedle =
    "a borrow-bearing record may not outlive the source of any borrow it "
    "contains";

}  // namespace

// ===========================================================================
// REJECT — §9.7 bullet 1, call-return form (issue #5627).
// ===========================================================================

// §9.7 (v0.326) / #5627: the CALL-RETURN shape — a call returning a
// borrow-bearing inline array of records (Helper.Wrap(b) returning [1]Str2)
// was not handled by carriedBorrowSourceOfValueExpr, so the outer-local
// escape check found no source and returned before the depth comparison.
// outer is at method scope (depth 0), b is in a do block (depth 1),
// td < sd (0 < 1) → reject.
BEZEL_TEST(own_borrow_lifetime_5627,
           call_return_borrow_bearing_record_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "end Box; "
        "record Str2(B: Box); "
        "class Helper "
        "  public static method Wrap(b: Box): [1]Str2 "
        "    return [new Str2(b)]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: [1]Str2; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      var b: Box := new Box(s); "
        "      outer := Helper.Wrap(b); "
        "    end; "
        "    return outer[0].B.V.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kRecordOutlivesNeedle));
}

// ===========================================================================
// ACCEPT — §9.7 bullet 1, same program with b hoisted to method scope.
// ===========================================================================

// §9.7 (v0.326) / #5627 control: b is declared at method scope (depth 0),
// outer is also at method scope (depth 0), td is NOT < sd → accepted.
// b holds "hello", outer[0].B.V is "hello", Length = 5.
BEZEL_TEST(own_borrow_lifetime_5627,
           call_return_borrow_bearing_record_hoisted_source_accepted) {
    auto r = runAotChecked(
        "module M; "
        "class Box "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "end Box; "
        "record Str2(B: Box); "
        "class Helper "
        "  public static method Wrap(b: Box): [1]Str2 "
        "    return [new Str2(b)]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: [1]Str2; "
        "    var b: Box; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      b := new Box(s); "
        "      outer := Helper.Wrap(b); "
        "    end; "
        "    return outer[0].B.V.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
