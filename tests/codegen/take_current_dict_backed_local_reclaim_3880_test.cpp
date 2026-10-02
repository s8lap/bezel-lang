// Codegen / leak — issue #3880 (§29.1 "`Current` semantics on borrowing and
// owning enumerators" / §28.9 / §28.11, v0.294): `TakeCurrent()`'s result on
// an `IOwningEnumerable<Set<T>>` (dict-backed) or `IOwningEnumerable<
// Dictionary<K,V>>` never reclaimed the backing for a `const`/plain-`var`
// binding — the same defect class fix #3878 closed for `Array<T>`, one level
// wider. Measured (byte-identical to the issue's own filing):
//
//     public static method Gen(): IOwningEnumerable<Set<int>>
//       var xs: Set<int> := [1, 2, 3];
//       yield return xs;
//     end Gen;
//     public static method Main(): int
//       ...
//       using var source := C.Gen();
//       using var en := source.GetOwningEnumerator();
//       while en.MoveNext() do
//         const s := en.TakeCurrent();
//         total := total + s.Count;
//       end;
//
// Pre-fix: 85600 B / 1200 allocations leaked over 200 drains
// (bz_dict_alloc_generic / dict_order_append / bz_dict_set_gen). The
// Dictionary<string,int> twin leaked 84400 B / 1200 allocations over 200
// drains (bz_dict_alloc / dict_order_append / bz_dict_set_str) — measured
// while writing this test, confirming the issue's "likely Dictionary<K,V>
// too" prediction, so it is folded into this same fix rather than filed
// separately.
//
// #3878's fix (`isFreshTakeCurrentBuiltinArrayLocal`) was deliberately
// narrowed to a builtin `Array<T>` result (name-head exactly "Array") and
// filed THIS issue rather than widening its string test: `Set<T>`/
// `Dictionary<K,V>` are dict-backed (`bz_dict_t*`), reclaimed via
// `bz_dict_free`, a DIFFERENT runtime call than `Array<T>`'s
// `bz_array_free` — admitting a dict-backed local into #3878's `candidates`/
// `borrowingArrayFreeEligible_` would run the ARRAY reclaim over a
// `bz_dict_t*` (a type-confused free, not merely a leak: LANE_RULES "never
// trade a leak for a double-free").
//
// THE FIX. A new independent-candidacy-test recognizer,
// `isFreshTakeCurrentDictBackedLocal` (`LlvmBackend_Setup.cpp`, beside its
// #3878 sibling), admits a `const`/plain-`var` bound to `<expr>.
// TakeCurrent()` at a `Dictionary<K,V>` result (any K/V — always
// bz_dict_t*-backed) or a dict-eligible `Set<T>` result (name-head exactly
// "Set", AND `genericBackingType(rt, 0, isSet=true, …)` non-null — a class/
// interface Set element is array-backed instead and is NOT claimed here, a
// distinct unmeasured gap). Matched locals feed a NEW eligibility set,
// `takeCurrentDictFreeEligible_`, populated by the SAME
// `collectBorrowingArrayFreeEligible` pass and the SAME `borrowingArrayMode`
// escape walk #3878 reuses — but consumed by a NEW `lowerLocalDecl` arm that
// calls `buildCollectionTempDisposal(slot, rt)` (the existing (slot,
// TypeRef) -> Disposal helper the CollectionLiteralExpr Set/Dict-literal
// arms and the owned-parameter auto-drop path already reuse), which derives
// the CORRECT bz_dict_free wiring — including the Set<string>
// dictStringKeys +1 release and any record/string dictionary-value disposer
// — from the local's own resolved type, rather than assuming a null
// per-value disposer the way the pre-existing ToDictionary()-materializer
// consumer (dictMaterializerFreeEligible_) does.
//
// ⚠ NEVER TRADE A LEAK FOR A DOUBLE-FREE. The receiver-type gate
// (`isIterFormatEnumeratorHead && isOwningIterableInterfaceHead`) is the
// IDENTICAL two-predicate combination `LlvmBackend_Calls2.cpp`'s explicit-
// TakeCurrent codegen arm (`isOwningEnumerator`, #1349 / §29.1) already uses
// to recognize this exact call shape, so the new recognizer can never admit
// a local whose initializer that arm did not actually lower this way. Rows
// 3-5 (Set) and 7-8 (Dictionary) are the soundness controls: `using var`
// must stay clean (already correct pre-fix via `registerUsingDisposal`,
// keyed on the BINDING FORM — a regression guard against a future
// double-free were the new predicate ever widened to `view.isUsing`), and a
// RETURNED / call-argument-passed local must remain leak-free AND
// crash-free WITHOUT the new predicate claiming them (the unmodified
// `borrowingArrayMode` escape walk this fix reuses already proves this for
// the #3878/#1871/#2142/#2143 sibling shapes; these rows re-verify it for
// the NEW dict-backed TakeCurrent() candidate specifically, in both
// directions per LANE_RULES: "a row that passes is not evidence until
// something has shown it can fail").
//
// Rows are `runAotChecked` (generated-code ASan with LeakSanitizer on),
// loop-amplified to 200 activations: a one-shot row reads clean on BOTH
// sides (the single allocation is still stack-reachable at exit), so it
// would prove nothing. per §28.9, §28.11, §29.1 (issue #3880)

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

// ── Row 1 (the issue's exact repro, `const`, Set<int>) — a `const s :=
// en.TakeCurrent();` binding, drained 200 times. Pre-fix: 85600 B / 1200
// allocations leaked (bz_dict_alloc_generic / dict_order_append /
// bz_dict_set_gen). 200 * 3 = 600.
// per §28.9, §28.11, §29.1 (issue #3880)
BEZEL_TEST(take_current_dict_backed_local_reclaim_3880,
           set_const_binding_backing_reclaimed_spec_29_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Set<int>> "
        "    var xs: Set<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        const s := en.TakeCurrent(); "
        "        total := total + s.Count; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 2 (plain `var` twin, Set<int>) — the issue's title also names a
// plain `var` binding as leaking identically (`isFreshTakeCurrentDictBacked
// Local` takes a `LocalDeclInitView`, admitting both node kinds via the
// shared #3402 `localDeclInitView` split). per §28.9, §28.11, §29.1, §21.1
// (issue #3880)
BEZEL_TEST(take_current_dict_backed_local_reclaim_3880,
           set_var_binding_backing_reclaimed_spec_29_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Set<int>> "
        "    var xs: Set<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        var s := en.TakeCurrent(); "
        "        total := total + s.Count; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 3 (CONTROL, Set<int> `using var` — must stay clean, no double-free)
// — the SAME call bound via `using var`, which already reclaimed correctly
// before this fix (via `registerUsingDisposal`, keyed on the BINDING FORM
// rather than the initializer shape). The new recognizer explicitly
// excludes `view.isUsing` (mirroring every sibling in the family), so this
// local is NEVER added to the new candidate set — this row guards against a
// future change accidentally widening the new predicate to `using var` and
// double-freeing a backing `registerUsingDisposal` already reclaims.
// per §28.9, §28.10, §28.11, §29.1 (issue #3880)
BEZEL_TEST(take_current_dict_backed_local_reclaim_3880,
           set_using_var_control_stays_clean_no_double_free_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Set<int>> "
        "    var xs: Set<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        using var s := en.TakeCurrent(); "
        "        total := total + s.Count; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 4 (NEGATIVE CONTROL — RETURN escape, Set<int>) — `TakeOne()` binds
// `s` via `const s := en.TakeCurrent();` and immediately `return`s it. The
// escape walk this fix reuses UNCHANGED (`StringEscapeWalker`'s `ReturnStmt`
// arm, unconditional for a bare tracked identifier) already treats a
// returned local as an escape for every sibling in the family — this row
// re-verifies it for the NEW dict-backed candidate: if the new predicate's
// node were wrongly inserted into `takeCurrentDictFreeEligible_` despite
// escaping, `TakeOne`'s scope-end free would run on the SAME backing the
// caller's `using var s := C.TakeOne();` then frees again — a
// double-free/heap-use-after-free `runAotChecked` would catch immediately,
// before the printed value is even compared. 200 * 3 = 600.
// per §28.1, §28.3, §28.9, §28.11, §29.1 (issue #3880)
BEZEL_TEST(take_current_dict_backed_local_reclaim_3880,
           set_returned_local_not_claimed_no_double_free_control) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Set<int>> "
        "    var xs: Set<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method TakeOne(): owned Set<int> "
        "    using var source := C.Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    en.MoveNext(); "
        "    const s := en.TakeCurrent(); "
        "    return s; "
        "  end TakeOne; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var s := C.TakeOne(); "
        "      total := total + s.Count; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 5 (NEGATIVE CONTROL — passed to another call, Set<int>) — `s` is
// passed as a plain (no-modifier) argument to `C.SumOf(s)`, then still
// reclaimed at the enclosing scope's end by this fix. `owned` cannot mark a
// `Set<T>` parameter at all (§28.1's type requirement restricts `owned` to
// class, interface, or bare-type-parameter declared types), so the closest
// available "handed onward through a call boundary" shape for a builtin
// collection is a plain-parameter pass, which the escape walk's existing
// `walkCallArgs` #2115c relaxation (`borrowingArrayMode`, unchanged by this
// fix) already treats as a CALL-BOUNDED BORROW for a resolved user method
// with a plain parameter — the caller remains the sole owner and this fix's
// scope-end free still fires exactly once. Verifies: no leak (the free
// still arms across the intervening call) and no double-free/UAF (`SumOf`'s
// own read happens before the free). 200 * 3 = 600.
// per §28.3, §28.9, §28.11, §29.1 (issue #3880)
BEZEL_TEST(take_current_dict_backed_local_reclaim_3880,
           set_passed_as_plain_call_argument_reclaimed_once_control) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Set<int>> "
        "    var xs: Set<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method SumOf(s: Set<int>): int "
        "    return s.Count; "
        "  end SumOf; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        const s := en.TakeCurrent(); "
        "        total := total + C.SumOf(s); "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 6 (Dictionary<string,int> twin of Row 1, `const`) — the issue's
// "likely Dictionary<K,V> too" prediction, measured while writing this test:
// 84400 B / 1200 allocations leaked pre-fix (bz_dict_alloc /
// dict_order_append / bz_dict_set_str — the STRING fast path, distinct
// runtime entry points from Set<int>'s GENERIC path in Row 1, exercising the
// `isFreshTakeCurrentDictBackedLocal` "Dictionary" arm, which — unlike the
// "Set" arm — needs no `genericBackingType` gate: a Dictionary is always
// bz_dict_t*-backed regardless of K/V). 200 * 3 = 600.
// per §28.9, §28.11, §29.1 (issue #3880)
BEZEL_TEST(take_current_dict_backed_local_reclaim_3880,
           dictionary_const_binding_backing_reclaimed_spec_29_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Dictionary<string, int>> "
        "    var xs: Dictionary<string, int> := [\"a\": 1, \"b\": 2, \"c\": 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        const d := en.TakeCurrent(); "
        "        total := total + d.Count; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 7 (CONTROL, Dictionary<string,int> `using var` — must stay clean,
// no double-free) — mirrors Row 3 for the Dictionary arm.
// per §28.9, §28.10, §28.11, §29.1 (issue #3880)
BEZEL_TEST(take_current_dict_backed_local_reclaim_3880,
           dictionary_using_var_control_stays_clean_no_double_free_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Dictionary<string, int>> "
        "    var xs: Dictionary<string, int> := [\"a\": 1, \"b\": 2, \"c\": 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        using var d := en.TakeCurrent(); "
        "        total := total + d.Count; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 8 (NEGATIVE CONTROL — RETURN escape, Dictionary<string,int>) —
// mirrors Row 4 for the Dictionary arm.
// per §28.1, §28.3, §28.9, §28.11, §29.1 (issue #3880)
BEZEL_TEST(take_current_dict_backed_local_reclaim_3880,
           dictionary_returned_local_not_claimed_no_double_free_control) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Dictionary<string, int>> "
        "    var xs: Dictionary<string, int> := [\"a\": 1, \"b\": 2, \"c\": 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method TakeOne(): owned Dictionary<string, int> "
        "    using var source := C.Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    en.MoveNext(); "
        "    const d := en.TakeCurrent(); "
        "    return d; "
        "  end TakeOne; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var d := C.TakeOne(); "
        "      total := total + d.Count; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}
