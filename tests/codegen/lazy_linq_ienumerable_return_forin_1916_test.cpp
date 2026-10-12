// Codegen/iteration — a lazy LINQ pipeline (`Select`/`Where`/… → a `bz_lazy_t*`)
// widened to a pure `IEnumerable<T>` in a method's RETURN position, then
// enumerated by the caller (issue #1916 — the RETURN sibling of #1911, stdlib
// §5 / §29.11).
//
// ROOT CAUSE (src/codegen/LlvmBackend_Assign.cpp wrapAsIEnumerableForTarget, the
// shared coercion the RETURN sites call). A lazy LINQ operator lowers to a
// runtime `bz_lazy_t*` whose vtable has only FOUR slots ([0]=td, [1]=Dispose,
// [2]=MoveNext, [3]=Current) and whose Current returns a POINTER to the element
// bytes. When such a value is returned as the pure interface `IEnumerable<T>`,
// the caller's for-in drives the §29.11 iterator-vtable protocol: it reads
// vtable slot 4 (GetEnumerator) — OUT OF BOUNDS for the 4-slot lazy vtable, so
// the loaded garbage word is called as a function -> pc≈0 SIGSEGV — and expects
// Current BY VALUE (the lazy Current returns a pointer). #1911 taught the
// local-decl init path (lowerLocalDecl) to wrap a lazy pipeline, but the RETURN
// sites route through wrapAsIEnumerableForTarget, which had NO lazy case — so a
// raw `bz_lazy_t*` was returned unwrapped and the caller's for-in crashed.
//
// FIX. wrapAsIEnumerableForTarget gained a lazy-sentinel branch (gated on a new
// `ownLazyPipeline` flag the four RETURN call sites pass) that drains the
// pipeline once into a fresh OWNED Array<T> (bz_lazy_to_array — which also
// disposes the pipeline, releasing its inline-lambda closures) and wraps that
// array in an OWNING vtable-backed enumerator (wrapOwnedArrayAsIEnumerable), via
// the shared drainLazyPipelineToOwnedIEnumerable helper #1911's lowerLocalDecl
// now also uses. The wrapper carries the full 5-slot §29.11 vtable
// (Current-by-value, GetEnumerator at slot 4 handing out NON-owning clones), so
// the caller's for-in dispatch is in-bounds and correct. The caller OWNS the
// returned IEnumerable: a for-in over the `Make()` CallExpr temporary disposes
// it loop-owned (the !sourceIsNamedLocal slot-1 Dispose path); a returned value
// bound to a local is disposed at the caller's scope end. Either way the owning
// wrapper's slot-1 Dispose frees the drained array exactly once — leak-free, no
// double-free. (The ARGUMENT position — a BORROWED callee `IEnumerable<T>`
// parameter — keeps the default `ownLazyPipeline=false` and is deferred in
// #1916; it needs the call-site arg-temporary disposal contract worked out
// first, so it is intentionally NOT covered here.)
//
// Fails-without / passes-with: before the fix the caller's for-in SIGSEGVs,
// which runAotChecked's ASan net turns into r.ran==false -> run()==-999 (never a
// legal value). After the fix the loop enumerates every element and every
// allocation is freed exactly once (verified ASan/LSan-clean by runAotChecked).
// The result element type is `int` throughout, so the drained snapshot owns no
// heap elements (any string source array disposes its own elements at its own
// scope end).
//
// Spec: stdlib §5 (deferred LINQ operators), §29 / §29.2 (for-in desugaring &
// element inference), §29.11 (vtable-backed IEnumerable<T>: slot-1 Dispose /
// slot-4 GetEnumerator). Spec revision v0.196.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// runAotChecked runs the linked executable under generated-code ASan; the pre-fix
// SIGSEGV (or any leak / double-free) flips r.ran to false -> -999, distinct from
// any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// stdlib §5 / §29.11 (#1916): the issue's canonical Repro A — a block-body
// method takes a source array as a PARAMETER (the caller owns it, so the #5624
// borrow-escape check does not fire), returns `xs.Select(...)` widened to
// `IEnumerable<int>`, and the caller enumerates the `Make()` CallExpr
// temporary directly (the loop-owned dispose path). The pipeline borrows the
// caller's array; the drain snapshots its values before the caller disposes the
// array at its scope end (no use-after-free). {2,3} +1 -> {3,4} -> sum 7.
// Before the fix: SIGSEGV (-999).  (The `using var` form `return
// xs.Select(lambda(n) => n + 1)` inside `Make` triggers the #5624 borrow-lifetime
// check — the spec's diagnostic directs "bind the source in the caller's scope
// and return the borrow from there instead"; the parameter form is that reshape.)
BEZEL_TEST(codegen_lazy_linq_ienumerable_return_forin_1916,
           select_returned_as_ienumerable_forin_temporary_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Make(xs: Array<int>): IEnumerable<int> "
        "    return xs.Select(lambda(n) => n + 1); "
        "  end Make; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [2, 3]; "
        "    var sum: int := 0; "
        "    for var x in Make(xs) do sum := sum + x; end for; "  // 3 + 4 = 7
        "    return sum; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}

// stdlib §5 / §29.11 (#1916): a lazy `Select` over a STRING source producing
// `int` lengths, returned (block body) as `IEnumerable<int>` and enumerated over
// the call temporary. The source is passed as a PARAMETER (the caller owns it;
// #5624 borrow-lifetime check does not fire). The returned int snapshot owns no
// heap elements. Lengths {2,3} -> sum 5. Before the fix: SIGSEGV (-999).
// (The `using var` form triggers #5624; the parameter form is used instead,
// mirroring `where_returned_via_expression_body_forin_spec_5`.)
BEZEL_TEST(codegen_lazy_linq_ienumerable_return_forin_1916,
           select_lengths_returned_as_ienumerable_forin_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Lens(strings: Array<string>): IEnumerable<int> "
        "    return strings.Select(lambda(s) => s.Length); "
        "  end Lens; "
        "  public static method Main(): int "
        "    var sum: int := 0; "
        "    for var x in Lens([\"ab\", \"cde\"]) do sum := sum + x; end for; "  // 2 + 3 = 5
        "    return sum; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 5);
}

// stdlib §5 (#1916): a lazy `Where` returned via an EXPRESSION-body method
// (`=> nums.Where(...)`) — the emitMethodBody return-coercion site
// (LlvmBackend_Setup.cpp), a different RETURN lowering than the block-body
// ReturnStmt. Of {1,2,3,4} keep n>2 -> {3,4} -> sum 7. Before the fix:
// SIGSEGV (-999).
BEZEL_TEST(codegen_lazy_linq_ienumerable_return_forin_1916,
           where_returned_via_expression_body_forin_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Big(nums: Array<int>): IEnumerable<int> "
        "      => nums.Where(lambda(n) => n > 2); "
        "  public static method Main(): int "
        "    using var nums: Array<int> := [1, 2, 3, 4]; "
        "    var sum: int := 0; "
        "    for var x in Big(nums) do sum := sum + x; end for; "  // 3 + 4 = 7
        "    return sum; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}

// §29.11 (#1916): a CHAINED lazy pipeline (`Where(...).Select(...)`) returned as
// `IEnumerable<int>` and bound to a caller local, then enumerated TWICE. The
// returned value materialises into a re-iterable owning snapshot, so the second
// for-in replays (GetEnumerator slot 4 hands out a fresh NON-owning clone each
// loop) and the caller's scope-end Dispose frees the drained array exactly once
// (a shared cursor or a double-freed clone/array would corrupt the heap ->
// -999). Of {1,2,3} keep n>1 -> {2,3}, *10 -> {20,30}; two loops -> 50 + 50 =
// 100. Before the fix the first loop already SIGSEGV'd.
BEZEL_TEST(codegen_lazy_linq_ienumerable_return_forin_1916,
           chained_pipeline_returned_bound_local_replays_spec_29_11) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Trans(xs: Array<int>): IEnumerable<int> "
        "    return xs.Where(lambda(n) => n > 1).Select(lambda(n) => n * 10); "
        "  end Trans; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3]; "
        "    var seq: IEnumerable<int> := Trans(xs); "
        "    var sum: int := 0; "
        "    for var x in seq do sum := sum + x; end for; "  // 20 + 30 = 50
        "    for var y in seq do sum := sum + y; end for; "  // replay -> +50
        "    return sum; "                                    // 100
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 100);
}

}  // namespace
