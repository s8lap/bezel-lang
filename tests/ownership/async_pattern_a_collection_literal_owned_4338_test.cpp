// fix #4338 (v0.298, §15.4 / §28.3 / §28.7): `producerState` had no arm for
// `ast::CollectionLiteralExpr`, so a local initialized by a heap-collection
// literal (`var arr: Array<int> := [1, 2, 3, 4]`) fell through to the default
// `Borrowed` classification. This caused the #4338 gate to reject a legal
// `return arr;` from an async method returning `Task<Array<int>>` — the
// checkAsyncPatternABorrowedIdentifierReturn gate fired when it should not have.
//
// Root cause: `producerState` defaults to `Borrowed` (the final
// `return OwnershipState::Borrowed`), and `CollectionLiteralExpr` had no arm.
// A heap-collection literal allocates fresh backing storage (§15.4) and the
// local carries the scope-end disposal obligation exactly like `new Array<T>()`
// does — the NewExpr arm already classifies that case Owned. The fix adds the
// missing arm using `isPatternAResultCollectionHead` (the same closed table
// codegen uses for Pattern A result disposal) to decide whether the literal's
// backing is heap-family or value-family.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation / diagnosticMessageContains
#include "support/aot_support.hpp"        // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §30.5 Pattern A: the REGRESSION — a local initialized by a collection literal
// and returned from an async method was misclassified as Borrowed and rejected
// by checkAsyncPatternABorrowedIdentifierReturn. The local's initializer was an
// Owned producer (heap backing, §15.4 / §28.3), so `return arr` is a legal
// owned-variable return. Must compile and run with exit 4.
BEZEL_TEST(async_pattern_a_collection_literal_owned_4338,
           bound_local_collection_literal_return) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeAsync(): Task<Array<int>> "
        "    var arr: Array<int> := [1, 2, 3, 4]; "
        "    return arr; "
        "  end MakeAsync; "
        "  public static method Main(): int "
        "    using var t := C.MakeAsync(); "
        "    t.Wait(); "
        "    return t.Result.Count; "  // 4
        "  end Main; "
        "end C;"), 4);
}

// §30.5 Pattern A — borrowed identifier return MUST STILL be rejected.
// A plain field read or parameter is a borrow source and cannot be returned
// from a Pattern A async method. This is NOT changed by the fix — the fix only
// corrects the collection-literal case above.
BEZEL_TEST(async_pattern_a_collection_literal_owned_4338,
           borrowed_identifier_still_rejected) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public item: Array<int>; "
        "  public constructor() this.item := new Array<int>(); end constructor; "
        "  public static async method MakeAsync(): Task<Array<int>> "
        "    return item; "  // bare field name, implicit `this.item` — a borrow
        "  end MakeAsync; "
        "  public static method Main(): int "
        "    using var t := C.MakeAsync(); "
        "    t.Wait(); "
        "    return t.Result.Count; "
        "  end Main; "
        "end C;");
    // Must fail to compile — a borrow cannot be returned from Pattern A.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// §30.5 / §15.4 / §28.3: NewExpr shape for the same Pattern A return — already
// worked via the NewExpr arm in producerState. The fix must not disturb it.
BEZEL_TEST(async_pattern_a_collection_literal_owned_4338,
           newexpr_shape_still_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeAsync(): Task<Array<int>> "
        "    var arr: Array<int> := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); arr.Add(3); "
        "    return arr; "
        "  end MakeAsync; "
        "  public static method Main(): int "
        "    using var t := C.MakeAsync(); "
        "    t.Wait(); "
        "    return t.Result.Count; "  // 3
        "  end Main; "
        "end C;"), 3);
}

// §15.4: synchronous return of a collection-literal-initialized local is
// unaffected — the fix is in producerState classification, not in any async-
// specific path. Must still compile and run with exit 5.
// §28.3 row 4 (v0.313, #5123): the static `Make` return is spelled
// `owned Array<int>` (a plain static Array return is rejected at the
// declaration); the sync-return-of-a-literal-local shape is otherwise intact.
BEZEL_TEST(async_pattern_a_collection_literal_owned_4338,
           sync_return_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Make(): owned Array<int> "
        "    var arr: Array<int> := [3, 9, 3, 1, 5]; "
        "    return arr; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var a := C.Make(); "
        "    return a.Count; "  // 5
        "  end Main; "
        "end C;"), 5);
}

// §15.4 / §28.3: Dictionary literal — same heap-backings, same Owned classification.
// Must compile and run with exit 2.
BEZEL_TEST(async_pattern_a_collection_literal_owned_4338,
           dict_collection_literal_return) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeAsync(): Task<Dictionary<string, int>> "
        "    var d: Dictionary<string, int> := [\"x\": 10, \"y\": 20]; "
        "    return d; "
        "  end MakeAsync; "
        "  public static method Main(): int "
        "    using var t := C.MakeAsync(); "
        "    t.Wait(); "
        "    return t.Result.Count; "  // 2
        "  end Main; "
        "end C;"), 2);
}
