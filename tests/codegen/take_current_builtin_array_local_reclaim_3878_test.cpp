// Codegen / leak — issue #3878 (§29.1 "`Current` semantics on borrowing and
// owning enumerators" / §28.9 / §28.11, v0.294): `TakeCurrent()`'s result on
// an `IOwningEnumerable<Array<T>>` (a builtin-collection-typed owning-
// iterator element) never reclaimed the array's backing for a `const`/plain-
// `var` binding — 24000 B / 400 allocations leaked over 200 drains,
// byte-identical to the issue's own measurement:
//
//     public static method Gen(): IOwningEnumerable<Array<int>>
//       var xs: Array<int> := [1, 2, 3];
//       yield return xs;
//     end Gen;
//     public static method Main(): int
//       ...
//       using var source := C.Gen();
//       using var en := source.GetOwningEnumerator();
//       while en.MoveNext() do
//         const arr := en.TakeCurrent();
//         total := total + arr.Count;
//       end;
//
// A `using var arr := en.TakeCurrent();` binding of the SAME call already
// reclaimed correctly (via `registerUsingDisposal`) — the gap was specific to
// the `const`/plain-`var` binding forms, whose initializer is a `CallExpr`
// (`.TakeCurrent()`), a shape neither `isBorrowingArrayLiteralLocal` nor
// `isFreshBorrowingArrayMaterializerLocal` recognized (both require a
// `CollectionLiteralExpr` or a `ToArray`/`Sorted`/`Reversed`/`Split`
// materializer call).
//
// §29.1 states `TakeCurrent()` "transfers ownership of the most-recently-
// yielded value to the caller... the result follows the standard §28.1/§28.3
// owned-return rules (discharge or auto-drop-with-warning — no special
// carve-out needed)" — i.e. `arr` is an ORDINARY owned local like any other.
// §28.9/§28.11 give `Array<T>`'s discharge story for a builtin (declaration-
// less) collection: `Dispose()` is a no-op (trivial-dispose), and the backing
// is "reclaimed with the instance" — for a plain `const`/`var` binding, that
// reclaim is a compiler-emitted scope-end `bz_array_free`, exactly what the
// `using var` form already got and the other two forms were missing.
//
// THE FIX. A new independent-candidacy-test recognizer,
// `isFreshTakeCurrentBuiltinArrayLocal` (`LlvmBackend_Setup.cpp`, alongside
// its `isFresh*TerminalLocal` siblings), feeds the SAME `candidates` vector
// `isFreshBorrowingArrayMaterializerLocal`/`isBorrowingArrayLiteralLocal`
// already populate — so it gets the identical escape walk
// (`collectBorrowingArrayFreeEligible`'s `borrowingArrayMode` pass) and the
// identical, already-generic `lowerLocalDecl` consumer (which derives the
// per-element disposer from the LOCAL's own resolved element type, not from
// which predicate populated the set) — no new consumption code. Narrowly
// scoped to a builtin `Array<T>` result (name-head exactly "Array"): a
// sibling gap for `Set<T>` (dict-backed) was measured while writing this
// test and filed separately as issue #3880 rather than folded in here
// speculatively (a `bz_dict_free` reclaim is a different codegen arm).
//
// ⚠ NEVER TRADE A LEAK FOR A DOUBLE-FREE. The receiver-type gate
// (`isIterFormatEnumeratorHead && isOwningIterableInterfaceHead`) is the
// SAME two-predicate combination `LlvmBackend_Calls2.cpp`'s explicit-
// TakeCurrent codegen arm (`isOwningEnumerator`, #1349 / §29.1) already uses
// to recognize this exact call shape, so the new recognizer can never admit
// a local whose initializer that arm did not actually lower this way. Rows
// 3-5 below are the soundness controls: `using var` must stay clean (already
// worked before this fix — regression guard against a future double-free),
// and a RETURNED / call-argument-passed `arr` must remain leak-free AND
// crash-free WITHOUT the new predicate claiming them (the escape walk this
// fix reuses, unmodified, already proves this for the sibling shapes; these
// rows re-verify it for the NEW candidate specifically).
//
// Rows are `runAotChecked` (generated-code ASan with LeakSanitizer on),
// loop-amplified to 200 activations: a one-shot row reads clean on BOTH
// sides (the single allocation is still stack-reachable at exit), so it
// would prove nothing. per §28.9, §28.11, §29.1 (issue #3878)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with
// LeakSanitizer on, so a leak, a double free, or a use-after-free fails the
// row (ran=false) before the printed value is even compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── Row 1 (the issue's exact repro, `const`) — a `const arr :=
// en.TakeCurrent();` binding, drained 200 times. Pre-fix: 24000 B / 400
// allocations leaked (`bz_array_alloc`, spine + data). 200 * 3 = 600.
// per §28.9, §28.11, §29.1 (issue #3878)
BEZEL_TEST(take_current_builtin_array_local_reclaim_3878,
           const_binding_backing_reclaimed_spec_29_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        const arr := en.TakeCurrent(); "
        "        total := total + arr.Count; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 2 (plain `var` twin) — the issue's title also names a plain `var`
// binding as leaking identically (`isFreshTakeCurrentBuiltinArrayLocal`
// takes a `LocalDeclInitView`, admitting both node kinds via the shared
// #3402 `localDeclInitView` split). per §28.9, §28.11, §29.1, §21.1
// (issue #3878)
BEZEL_TEST(take_current_builtin_array_local_reclaim_3878,
           var_binding_backing_reclaimed_spec_29_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        var arr := en.TakeCurrent(); "
        "        total := total + arr.Count; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 3 (CONTROL, `using var` — must stay clean, no double-free) — the
// SAME call bound via `using var`, which already reclaimed correctly before
// this fix (via `registerUsingDisposal`, keyed on the BINDING FORM rather
// than the initializer shape). This fix's new recognizer explicitly excludes
// `view.isUsing` (mirroring every sibling in the family), so this local is
// NEVER added to the new candidate set — this row guards against a future
// change accidentally widening the new predicate to `using var` and double-
// freeing a backing `registerUsingDisposal` already reclaims.
// per §28.9, §28.10, §28.11, §29.1 (issue #3878)
BEZEL_TEST(take_current_builtin_array_local_reclaim_3878,
           using_var_control_stays_clean_no_double_free_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        using var arr := en.TakeCurrent(); "
        "        total := total + arr.Count; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 4 (NEGATIVE CONTROL — RETURN escape) — `TakeOne()` binds `arr` via
// `const arr := en.TakeCurrent();` and immediately `return`s it. §28.1's
// bare-type-parameter erasure rule (`TakeCurrent(): owned T` at `T =
// Array<int>`, neither class/interface nor move-only) makes the SUBSTITUTED
// signature unmarked, so `TakeOne(): Array<int>` (no `owned` keyword — that
// spelling would itself be a compile error for a non-class/interface/bare-
// type-param result, §28.1's type requirement) is ordinary syntax; the
// caller still inherits the discharge obligation the erasure note describes
// ("no obligation exists to transfer" is about `Dispose()` ceremony, not
// about the backing-reclaim story #3878/§28.11 covers).
//
// The escape walk this fix reuses UNCHANGED (`StringEscapeWalker`'s
// `ReturnStmt` arm: `expr(r->value.get(), /*safeRead=*/false)`, unconditional
// for a bare tracked identifier) already treats a returned local as an
// escape for every sibling in the `candidates` family — this row re-verifies
// it for the NEW `TakeCurrent`-initialized candidate specifically: if the
// new predicate's node were wrongly inserted into `borrowingArrayFreeEligible_`
// despite escaping, `TakeOne`'s scope-end free would run on the SAME backing
// the caller's `using var arr := C.TakeOne();` then frees again — a
// double-free/heap-use-after-free `runAotChecked` would catch immediately,
// before the printed value is even compared. 200 * 3 = 600.
// per §28.1, §28.3, §28.9, §28.11, §29.1 (issue #3878)
BEZEL_TEST(take_current_builtin_array_local_reclaim_3878,
           returned_local_not_claimed_no_double_free_control) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method TakeOne(): Array<int> "
        "    using var source := C.Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    en.MoveNext(); "
        "    const arr := en.TakeCurrent(); "
        "    return arr; "
        "  end TakeOne; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var arr := C.TakeOne(); "
        "      total := total + arr.Count; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 5 (NEGATIVE CONTROL — passed to another call) — `arr` is passed as
// a plain (no-modifier) argument to `C.SumOf(arr)`, then still reclaimed at
// the enclosing scope's end by this fix. `owned` cannot mark an `Array<T>`
// parameter at all (§28.1's type requirement restricts `owned` to class,
// interface, or bare-type-parameter declared types — an `owned xs:
// Array<int>` parameter is itself a compile error), so the closest available
// "handed onward through a call boundary" shape for a builtin collection is
// a plain-parameter pass, which the escape walk's existing `walkCallArgs`
// #2115c relaxation (`borrowingArrayMode`, unchanged by this fix) already
// treats as a CALL-BOUNDED BORROW for a resolved user method with a plain
// parameter — the callee reads `xs.Count` and returns; it does not alias
// the backing past the call, so the CALLER remains the sole owner and this
// fix's scope-end free still fires exactly once. Verifies: no leak (the
// free still arms across the intervening call) and no double-free/UAF
// (`SumOf`'s own read happens before the free). 200 * 3 = 600.
// per §28.3, §28.9, §28.11, §29.1 (issue #3878)
BEZEL_TEST(take_current_builtin_array_local_reclaim_3878,
           passed_as_plain_call_argument_reclaimed_once_control) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method SumOf(xs: Array<int>): int "
        "    return xs.Count; "
        "  end SumOf; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        const arr := en.TakeCurrent(); "
        "        total := total + C.SumOf(arr); "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}
