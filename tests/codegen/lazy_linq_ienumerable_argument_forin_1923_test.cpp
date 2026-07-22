// Codegen/iteration — a lazy LINQ pipeline (`Select`/`Where`/… → a `bz_lazy_t*`)
// widened to a pure `IEnumerable<T>` in ARGUMENT position, then enumerated by the
// callee (issue #1923 — the deferred ARGUMENT half of #1916, sibling of #1911;
// stdlib §5 / §29.11 / §28.3).
//
// ROOT CAUSE (src/codegen/LlvmBackend_Assign.cpp buildOrderedArgs, the call-site
// arg wrap). A lazy LINQ operator lowers to a runtime `bz_lazy_t*` whose vtable
// has only FOUR slots ([0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current) and whose
// Current returns a POINTER to the element bytes. When such a value is passed as
// the pure interface `IEnumerable<T>` argument, the callee's for-in drives the
// §29.11 iterator-vtable protocol: it reads vtable slot 4 (GetEnumerator) — OUT
// OF BOUNDS for the 4-slot lazy vtable, so the loaded garbage word is called as
// a function -> pc≈0 SIGSEGV — and expects Current BY VALUE. #1911 fixed the
// local-decl init path and #1916 fixed the RETURN sites (both routing through
// wrapAsIEnumerableForTarget's `ownLazyPipeline` branch), but the ARGUMENT site
// kept the default `ownLazyPipeline=false` and still passed the raw `bz_lazy_t*`
// unwrapped -> the callee's for-in crashed.
//
// FIX. buildOrderedArgs now detects a lazy pipeline passed as a pure
// `IEnumerable<T>` argument and drains it once into a fresh OWNED Array<T>
// (bz_lazy_to_array — which also disposes the pipeline, releasing its
// inline-lambda closures) wrapped in an OWNING vtable-backed enumerator
// (wrapAsIEnumerableForTarget with ownLazyPipeline=true). The wrapper carries the
// full 5-slot §29.11 vtable (Current-by-value, GetEnumerator at slot 4 handing
// out NON-owning clones), so the callee's for-in dispatch is in-bounds.
//
// DISPOSAL CONTRACT (why the ARGUMENT half was deferred from #1916). A callee
// `IEnumerable<T>` parameter is BORROWED — the callee's for-in takes the
// no-dispose sourceIsNamedLocal path and never disposes it, and borrow-checking
// (§28.3) forbids the callee from retaining it past the call. So the owning
// enumerator is a CALLER temporary the CALLER must dispose exactly once, after
// the call, on BOTH the normal-return and the exception-unwind edges — else the
// drained array leaks. buildOrderedArgs registers it as a scope-end
// iterEnumDispose (the same slot-1 Dispose #1911's local-decl source uses): its
// slot-1 Dispose frees the drained array AND the enumerator frame, null-guarded,
// exactly once. It is NOT unwindOnly and NOT in argTempSlots, so the pre-call
// discharge leaves it intact and it fires at scope end on the normal edge; a
// LATER argument (or the callee itself) that throws unwinds through this scope
// and fires it on the exception edge. Either way: exactly once, no double-free
// (the callee never disposes a borrowed arg), no leak. Mirrors the #1019
// borrow-parameter `new T(...)` arg-temporary disposal.
//
// Fails-without / passes-with: before the fix the callee's for-in SIGSEGVs, which
// runAotChecked's ASan net turns into r.ran==false -> run()==-999 (never a legal
// value). After the fix the loop enumerates every element and every allocation is
// freed exactly once (verified ASan/LSan-clean by runAotChecked), including on
// the two exception edges.
//
// Spec: stdlib §5 (deferred LINQ operators), §29 / §29.2 (for-in desugaring &
// element inference), §29.11 (vtable-backed IEnumerable<T>: slot-1 Dispose /
// slot-4 GetEnumerator), §28.3 (borrowed callee param + arg-temporary disposal on
// the exception edge). Spec revision v0.197.

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

// stdlib §5 / §29.11 (#1923): the issue's canonical repro — `Sum(xs.Select(...))`
// passes a lazy `Select` pipeline as the pure `IEnumerable<int>` parameter and
// the callee enumerates it. {2,3} +1 -> {3,4} -> sum 7. Before the fix: the
// callee's for-in SIGSEGVs (-999).
BEZEL_TEST(codegen_lazy_linq_ienumerable_argument_forin_1923,
           select_passed_as_ienumerable_argument_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Sum(seq: IEnumerable<int>): int "
        "    var s: int := 0; "
        "    for var x in seq do s := s + x; end for; "
        "    return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := new Array<int>(); "
        "    xs.Add(2); xs.Add(3); "
        "    return Sum(xs.Select(lambda(n) => n + 1)); "  // 3 + 4 = 7
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}

// stdlib §5 (#1923): a lazy `Where` passed as an `IEnumerable<int>` argument. Of
// {1,2,3,4} keep n>2 -> {3,4} -> sum 7. Before the fix: SIGSEGV (-999).
BEZEL_TEST(codegen_lazy_linq_ienumerable_argument_forin_1923,
           where_passed_as_ienumerable_argument_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Sum(seq: IEnumerable<int>): int "
        "    var s: int := 0; "
        "    for var x in seq do s := s + x; end for; "
        "    return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3, 4]; "
        "    return Sum(xs.Where(lambda(n) => n > 2)); "  // 3 + 4 = 7
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}

// §29.11 (#1923): a CHAINED lazy pipeline (`Where(...).Select(...)`) passed as an
// `IEnumerable<int>` argument. Of {1,2,3} keep n>1 -> {2,3}, *10 -> {20,30} ->
// sum 50. Exercises a multi-stage pipeline drained at the call site; a shared
// cursor / double-freed clone would corrupt the heap -> -999.
BEZEL_TEST(codegen_lazy_linq_ienumerable_argument_forin_1923,
           chained_pipeline_passed_as_argument_spec_29_11) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Sum(seq: IEnumerable<int>): int "
        "    var s: int := 0; "
        "    for var x in seq do s := s + x; end for; "
        "    return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3]; "
        "    return Sum(xs.Where(lambda(n) => n > 1).Select(lambda(n) => n * 10)); "  // 20 + 30 = 50
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 50);
}

// stdlib §5 / §29.11 (#1923): a lazy `Select` over a STRING source producing
// `int` lengths, passed as an `IEnumerable<int>` argument. The caller's `using
// var strings` (holding the heap strings) is disposed once at Main's scope end;
// the drained int snapshot owns no heap elements. Lengths {2,3} -> sum 5. Before
// the fix: SIGSEGV (-999).
BEZEL_TEST(codegen_lazy_linq_ienumerable_argument_forin_1923,
           select_lengths_passed_as_argument_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Sum(seq: IEnumerable<int>): int "
        "    var s: int := 0; "
        "    for var x in seq do s := s + x; end for; "
        "    return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var strings: Array<string> := [\"ab\", \"cde\"]; "
        "    return Sum(strings.Select(lambda(s) => s.Length)); "  // 2 + 3 = 5
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 5);
}

// §28.3 (#1923) — CALLEE-THROWS exception edge. The callee borrows the drained
// enumerator and throws PART-WAY through enumerating it. The drained-array
// temporary must be disposed exactly once as the exception unwinds through the
// caller's `try` scope (scope-end iterEnumDispose on the unwind edge), with NO
// leak and NO double-free (the callee never disposes a borrowed arg). Handler
// runs -> caught -> 42; a leak or double-free flips runAotChecked to -999.
BEZEL_TEST(codegen_lazy_linq_ienumerable_argument_forin_1923,
           callee_throws_while_borrowing_disposes_arg_temp_once_spec_28_3) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method SumOrThrow(seq: IEnumerable<int>): int "
        "    for var x in seq do "
        "      if x > 2 then throw new Exception(\"boom\"); end if; "
        "    end for; "
        "    return 0; "
        "  end SumOrThrow; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := new Array<int>(); "
        "    xs.Add(1); xs.Add(2); xs.Add(3); "
        "    var caught: bool := false; "
        "    try "
        "      var r: int := SumOrThrow(xs.Select(lambda(n) => n + 1)); "
        "    catch Exception => do caught := true; end "
        "    end; "
        "    return (if caught then 42 else 0); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 42);
}

// §28.3 (#1923) — LATER-ARGUMENT-THROWS exception edge. The first argument's lazy
// pipeline is drained into an owning-enumerator temporary in source order; then a
// LATER argument (`Boom()`) throws BEFORE the call runs. Control leaves the
// argument list without invoking the callee, so the already-drained temporary is
// orphaned — it must be disposed exactly once as the exception unwinds through
// the `try` scope (the temporary is registered as a non-unwindOnly scope-end
// iterEnumDispose, left intact by the pre-call discharge), with NO leak. Handler
// runs -> caught -> 42; a leak flips runAotChecked to -999.
BEZEL_TEST(codegen_lazy_linq_ienumerable_argument_forin_1923,
           later_argument_throws_disposes_drained_temp_once_spec_28_3) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Sum2(seq: IEnumerable<int>, k: int): int "
        "    var s: int := k; "
        "    for var x in seq do s := s + x; end for; "
        "    return s; "
        "  end Sum2; "
        "  public static method Boom(): int "
        "    throw new Exception(\"boom\"); "
        "  end Boom; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := new Array<int>(); "
        "    xs.Add(2); xs.Add(3); "
        "    var caught: bool := false; "
        "    try "
        "      var r: int := Sum2(xs.Select(lambda(n) => n + 1), Boom()); "
        "    catch Exception => do caught := true; end "
        "    end; "
        "    return (if caught then 42 else 0); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 42);
}

// stdlib §5 / §29.11 / §1226 (#1923): a lazy pipeline first BOUND to an inferred
// local (`var q := xs.Select(...)`, whose static type is the lazy sentinel) and
// then passed as the `IEnumerable<int>` argument. buildOrderedArgs drains `q` at
// the call site (consuming the affine pipeline, #1226); the drain both disposes
// the pipeline and owns the resulting array, disposed once at scope end — no
// double-dispose of `q`'s pipeline. {2,3} +1 -> {3,4} -> 7. Before the fix:
// SIGSEGV (-999).
BEZEL_TEST(codegen_lazy_linq_ienumerable_argument_forin_1923,
           inferred_lazy_local_passed_as_argument_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Sum(seq: IEnumerable<int>): int "
        "    var s: int := 0; "
        "    for var x in seq do s := s + x; end for; "
        "    return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := new Array<int>(); "
        "    xs.Add(2); xs.Add(3); "
        "    var q := xs.Select(lambda(n) => n + 1); "
        "    return Sum(q); "  // 3 + 4 = 7
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}

}  // namespace
