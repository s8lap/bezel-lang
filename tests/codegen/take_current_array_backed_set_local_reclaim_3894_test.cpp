// Codegen / leak — issue #3894 (§29.1 "`Current` semantics on borrowing and
// owning enumerators" / §28.9 / §28.11, v0.294): `TakeCurrent()`'s result on
// an `IOwningEnumerable<Set<C>>` where `C` is a CLASS element — i.e. an
// ARRAY-BACKED `Set<T>`, not a dict-backed one — never reclaimed the array
// backing for a `const`/plain-`var` binding. Claimed by NEITHER of the two
// existing recognizers in the family:
//
//   - `fix #3878`'s Array arm (`isFreshTakeCurrentBuiltinArrayLocal`)
//     requires `builtinTypeName(rt) == "Array"` exactly. A `Set<Item>` has
//     head name "Set", so it fails this gate.
//   - `fix #3880`'s dict-backed arm (`isFreshTakeCurrentDictBackedLocal`)
//     requires `genericBackingType(rt, 0, isSet=true, …)` to be non-null,
//     i.e. dict-eligibility (record / non-nullable primitive-or-enum /
//     string element). A CLASS element is never dict-eligible
//     (`genericBackingType` declines every class — the SAME exclusion
//     `materializeArrayBackedSetLiteral`'s own header comment documents,
//     issue #1070), so it fails this gate too.
//
//     public static method Gen(): IOwningEnumerable<Set<Item>>
//       using var a := new Item(5);
//       using var b := new Item(6);
//       var xs: Set<Item> := [a, b];
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
// So a `const s := en.TakeCurrent();` of such a value leaks its `bz_array_t*`
// backing (the Set's own spine + data buffer — NOT the two `Item` elements,
// which stay owned by their own `using var a`/`using var b` locals inside
// `Gen`, exactly as `class_element_set_literal_1070_test.cpp`'s
// `borrowing_set_literal_no_dispose` row establishes: "the BORROWING `Set<C>`
// disposes nothing — its elements are borrows").
//
// THE FIX. A third independent-candidacy-test recognizer,
// `isFreshTakeCurrentArrayBackedSetLocal` (`LlvmBackend_Setup.cpp`, beside
// its two #3878/#3880 siblings), admits a `const`/plain-`var` bound to
// `<expr>.TakeCurrent()` at a builtin `Set<T>` result (name-head exactly
// "Set", not "OwningSet"/"SortedSet") whose element is NOT generic-dict-
// eligible (`genericBackingType(rt, 0, isSet=true, …)` null) — the EXACT
// complement of #3880's Set arm. This shape frees via `bz_array_free`, the
// SAME runtime call `Array<T>`'s TakeCurrent reclaim uses, so it feeds the
// SAME `candidates` vector / `borrowingArrayFreeEligible_` set #3878's fix
// populates — NOT a new eligibility set, and therefore NO new save/restore
// mirroring is needed (every state-machine site the family requires already
// mirrors `borrowingArrayFreeEligible_` itself). The existing, already-
// generic `lowerLocalDecl` consumer for that set derives the per-element
// disposer from the local's own resolved element type via
// `elementPolicyFor(el, /*owningFamily=*/false)`, which is null for a
// borrowing class element (only the backing is reclaimed, §28.11) —
// byte-identical to what `buildCollectionTempDisposal`'s own array-backed
// fallback branch derives for this same `TypeRef` (its header comment: "A
// class/interface/nullable element is NOT dict-eligible (genericBackingType
// returns null) → that Set is array-backed and falls through to
// bz_array_free below").
//
// SPEC. §29.1: `TakeCurrent()` "transfers ownership of the most-recently-
// yielded value to the caller ... the result follows the standard §28.1/
// §28.3 owned-return rules" — `s` is an ORDINARY owned local. §28.9/§28.11:
// a builtin `Set<T>` is trivial-dispose and its backing is "reclaimed with
// the instance" — for a plain `const`/`var` binding that reclaim is the
// scope-end `bz_array_free` this fix adds.
//
// ⚠ NEVER TRADE A LEAK FOR A DOUBLE-FREE. The receiver-type gate
// (`isIterFormatEnumeratorHead && isOwningIterableInterfaceHead`) is the
// SAME two-predicate combination `LlvmBackend_Calls2.cpp`'s explicit-
// TakeCurrent codegen arm (`isOwningEnumerator`, #1349 / §29.1) already uses
// to recognize this exact call shape, so the new recognizer can never admit
// a local whose initializer that arm did not actually lower this way. Rows
// 3-5 are the soundness controls: `using var` must stay clean (already
// correct pre-fix via `registerUsingDisposal`, keyed on the BINDING FORM —
// a regression guard against a future double-free were the new predicate
// ever widened to `view.isUsing`), and a RETURNED / call-argument-passed
// local must remain leak-free AND crash-free WITHOUT the new predicate
// claiming them (the unmodified escape walk the #3878/#3880 family already
// reuses proves this for the sibling shapes; these rows re-verify it for
// THIS candidate specifically, per LANE_RULES: "a row that passes is not
// evidence until something has shown it can fail").
//
// Rows are `runAotChecked` (generated-code ASan with LeakSanitizer on),
// loop-amplified to 200 activations: a one-shot row reads clean on BOTH
// sides (the single allocation is still stack-reachable at exit), so it
// would prove nothing. per §28.9, §28.11, §29.1 (issue #3894)

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

// A plain class element: §8.3 reference identity (no Equals override) —
// verbatim the `kItem` shape `class_element_set_literal_1070_test.cpp`
// already established as the minimal disposable class-element fixture.
const char* kItem =
    "module M; class Item "
    "  public V: int := 0; "
    "  public constructor(v: int) this.V := v; end constructor; "
    "  destructor end destructor; "
    "end Item; class C ";
}  // namespace

// ── Row 1 (the issue's exact repro, `const`) — a `const s :=
// en.TakeCurrent();` binding, drained 200 times. Each drained Set<Item> holds
// 2 borrowed items (owned by Gen's own `using var a`/`using var b`, disposed
// when Gen's generator frame completes) — the ARRAY BACKING (spine + data)
// is what leaks pre-fix. 200 * 2 = 400.
// per §28.9, §28.11, §29.1 (issue #3894)
BEZEL_TEST(take_current_array_backed_set_local_reclaim_3894,
           const_binding_backing_reclaimed_spec_29_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kItem) +
        "  public static method Gen(): IOwningEnumerable<Set<Item>> "
        "    using var a := new Item(5); "
        "    using var b := new Item(6); "
        "    var xs: Set<Item> := [a, b]; "
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
        "total=400\n");
}

// ── Row 2 (plain `var` twin) — the issue's title also names a plain `var`
// binding as leaking identically (`isFreshTakeCurrentArrayBackedSetLocal`
// takes a `LocalDeclInitView`, admitting both node kinds via the shared
// #3402 `localDeclInitView` split). per §28.9, §28.11, §29.1, §21.1
// (issue #3894)
BEZEL_TEST(take_current_array_backed_set_local_reclaim_3894,
           var_binding_backing_reclaimed_spec_29_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kItem) +
        "  public static method Gen(): IOwningEnumerable<Set<Item>> "
        "    using var a := new Item(5); "
        "    using var b := new Item(6); "
        "    var xs: Set<Item> := [a, b]; "
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
        "total=400\n");
}

// ── Row 3 (CONTROL, `using var` — must stay clean, no double-free) — the
// SAME call bound via `using var`, which already reclaimed correctly before
// this fix (via `registerUsingDisposal`, keyed on the BINDING FORM rather
// than the initializer shape). The new recognizer explicitly excludes
// `view.isUsing` (mirroring every sibling in the family), so this local is
// NEVER added to the new candidate set — this row guards against a future
// change accidentally widening the new predicate to `using var` and
// double-freeing a backing `registerUsingDisposal` already reclaims.
// per §28.9, §28.10, §28.11, §29.1 (issue #3894)
BEZEL_TEST(take_current_array_backed_set_local_reclaim_3894,
           using_var_control_stays_clean_no_double_free_spec_28_10) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kItem) +
        "  public static method Gen(): IOwningEnumerable<Set<Item>> "
        "    using var a := new Item(5); "
        "    using var b := new Item(6); "
        "    var xs: Set<Item> := [a, b]; "
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
        "total=400\n");
}

// ── Row 4 (NEGATIVE CONTROL — RETURN escape) — `TakeOne()` binds `s` via
// `const s := en.TakeCurrent();` and immediately `return`s it. The escape
// walk this fix reuses UNCHANGED (`StringEscapeWalker`'s `ReturnStmt` arm,
// unconditional for a bare tracked identifier) already treats a returned
// local as an escape for every sibling in the family — this row re-verifies
// it for the NEW array-backed-Set candidate specifically: if the new
// predicate's node were wrongly inserted into `borrowingArrayFreeEligible_`
// despite escaping, `TakeOne`'s scope-end free would run on the SAME
// backing the caller's `using var s := C.TakeOne();` then frees again — a
// double-free/heap-use-after-free `runAotChecked` would catch immediately,
// before the printed value is even compared. 200 * 2 = 400.
// per §28.1, §28.3, §28.9, §28.11, §29.1 (issue #3894)
BEZEL_TEST(take_current_array_backed_set_local_reclaim_3894,
           returned_local_not_claimed_no_double_free_control) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kItem) +
        "  public static method Gen(): IOwningEnumerable<Set<Item>> "
        "    using var a := new Item(5); "
        "    using var b := new Item(6); "
        "    var xs: Set<Item> := [a, b]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method TakeOne(): Set<Item> "
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
        "total=400\n");
}

// ── Row 5 (NEGATIVE CONTROL — passed to another call) — `s` is passed as a
// plain (no-modifier) argument to `C.SumOf(s)`, then still reclaimed at the
// enclosing scope's end by this fix. `owned` cannot mark a `Set<T>`
// parameter at all (§28.1's type requirement restricts `owned` to class,
// interface, or bare-type-parameter declared types), so the closest
// available "handed onward through a call boundary" shape for a builtin
// collection is a plain-parameter pass, which the escape walk's existing
// `walkCallArgs` #2115c relaxation (`borrowingArrayMode`, unchanged by this
// fix) already treats as a CALL-BOUNDED BORROW for a resolved user method
// with a plain parameter — the caller remains the sole owner and this fix's
// scope-end free still fires exactly once. Verifies: no leak (the free
// still arms across the intervening call) and no double-free/UAF (`SumOf`'s
// own read happens before the free). 200 * 2 = 400.
// per §28.3, §28.9, §28.11, §29.1 (issue #3894)
BEZEL_TEST(take_current_array_backed_set_local_reclaim_3894,
           passed_as_plain_call_argument_reclaimed_once_control) {
    BEZEL_EXPECT_EQ(checkedOut(
        std::string(kItem) +
        "  public static method Gen(): IOwningEnumerable<Set<Item>> "
        "    using var a := new Item(5); "
        "    using var b := new Item(6); "
        "    var xs: Set<Item> := [a, b]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method SumOf(s: Set<Item>): int "
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
        "total=400\n");
}
