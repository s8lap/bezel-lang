// Codegen/runtime — issue #2151 (§28.3 / stdlib §5): a chained
// `X.Split(",").ToDictionary(...)` (or any fresh-array-materializer receiver
// feeding a terminal LINQ consumer) leaked the intermediate `Split`
// `Array<string>` temporary.
//
// Root cause (src/codegen/LlvmBackend_Calls3.cpp, lowerToLazyEnumerator + the
// ToDictionary interception). The Split result (a fresh bz_array_t*) is
// wrapped as a BORROWING lazy source via bz_lazy_from_array; bz_lazy_to_dict
// consumes the lazy via bz_lazy_dispose, but `bz_lazy_dispose` only frees
// arrays it OWNS (own_arr set by bz_lazy_from_owned_array), not borrowed
// ones — the borrowed array is the caller's responsibility. The using-Array
// reclaim path (borrowingArrayFreeEligibleFn_) only fires for NAMED `var`
// locals (registered in lowerLocalDecl); an UNBOUND temporary receiver is
// dropped, not bound, so the reclaim gate never matched and the array (and
// every substring) leaked.
//
// Fix. Surface the lowered receiver array out of lowerToLazyEnumerator (new
// `outRecvArr` output param), and in the ToDictionary codegen immediately
// after `bz_lazy_to_dict` consumes the lazy, call freeUnboundOwnedBorrowing-
// ArrayTemp on the array. The free is gated on `isFreshOwnerlessArrayMateri-
// alizerExpr` / `isUnboundOwnedBorrowingArrayCallExpr` so a named-local /
// `using var` / `move`d receiver is NEVER re-freed (its owner disposes it —
// no double-free, no UAF).
//
// Spec:
//   §28.3 — unbound owned/borrowing temporaries reclaimed at end-of-statement.
//   §28.9 / §28.11 — borrowing-collection backing "reclaimed with the instance".
//   stdlib §5 — `Array<T>` is a borrowing snapshot; caller owns the array, not
//               its elements.
//
// fail-without/pass-with. Without the fix each chained-materializer receiver
// leaks (LSan → ran=false → the -999 sentinel); with the fix every program
// is ASan/LSan-clean and returns the expected value.

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked returns ran=false on any sanitizer fire (an LSan leak on
// the unbound intermediate at end-of-statement); map that to a sentinel so
// a single EXPECT_EQ pins BOTH memory-cleanliness AND the computed value.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Primary repro (issue body): sync method, non-iterator, chained
// `Split(",").ToDictionary(sel)` leaks the intermediate Split Array<string>
// (209 B, 8 allocations — the spine + every substring). Yields the dict's
// Count (3 entries: "a"=1, "bb"=2, "ccc"=3). // per §28.3
BEZEL_TEST(chain_split_todict_2151, split_to_dict_chained_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Compute(): int "
        "    var d := \"a,bb,ccc\".Split(\",\").ToDictionary("
        "        lambda(s: string) => s.Length); "
        "    return d.Count; "
        "  end Compute; "
        "  public static method Main(): int "
        "    return C.Compute(); "
        "  end Main; "
        "end C;"), 3);
}

// Control: a NAMED `using var` binding the Split result first. The using-var
// path is already correct (the borrowing-Array-eligibility walk in
// lowerLocalDecl arms a scope-end bz_array_free), and the new fix MUST NOT
// double-free — the ToDictionary receiver is no longer a CallExpr (it's a
// LocalExpr) so `isFreshOwnerlessArrayMaterializerExpr` rejects it.
BEZEL_TEST(chain_split_todict_2151, bound_split_local_not_double_freed) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Compute(): int "
        "    using var xs := \"a,bb\".Split(\",\"); "
        "    var d := xs.ToDictionary(lambda(s: string) => s.Length); "
        "    return d.Count; "
        "  end Compute; "
        "  public static method Main(): int "
        "    return C.Compute(); "
        "  end Main; "
        "end C;"), 2);
}

// Control: an integer Array<string> chain — the same Split() materializer —
// feeding an eager COUNT terminal (existing path; Count's interception in
// tryLowerCollectionInstanceCall already calls freeUnboundOwnedBorrowing-
// ArrayTemp at line ~5016, so this case was already leak-clean before the
// fix and is kept as a green control.
BEZEL_TEST(chain_split_todict_2151, split_count_unchanged_green_control) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Compute(): int "
        "    return \"a,bb,ccc\".Split(\",\").Count; "
        "  end Compute; "
        "  public static method Main(): int "
        "    return C.Compute(); "
        "  end Main; "
        "end C;"), 3);
}