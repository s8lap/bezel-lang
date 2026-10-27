// Codegen / SEGV — fix #3355 (§29.2 / §23.2, v0.278):
// the BLOCK-bodied `IOwningEnumerable<T>` PROPERTY GETTER over a non-Array
// source returned the raw backing pointer instead of a vtable-backed
// wrapper. The caller's `for x in e` drove the §29.2 vtable dispatch on
// the raw backing descriptor and read slot 4 (GetOwningEnumerator) as a
// pointer-sized field of the backing — and called THAT as a function →
// SIGSEGV (the pre-fix shape crashes identically to #2815's `=> _items;`
// over an `IEnumerable<T>` property, because both come from the same
// property-read emit path that has no IOwningEnumerable non-Array wrap
// variant; the IEnumerable family is handled at #2815 / #2703 / #2777 /
// #2987 / #2299, but the IOwningEnumerable arm was a no-op fallback
// through `wrapAsIEnumerableForTarget` — its gate keyed on
// `headName() == "IEnumerable"` and declined every other name). #2769's
// commentary at line 193 names this defect explicitly: "the
// IOwningEnumerable arm of the wrap function itself is broken upstream
// of #2769's scope. Filed as a separate issue" — that issue is #3355.
//
// THE FIX (this commit — INTERIM, SIGSEGV-elimination). Generalize the
// gate at `wrapAsIEnumerableForTarget` line 7561-7594 to ALSO accept
// `IOwningEnumerable` as the target head. The downstream dispatch reuses
// the existing wrap helpers (`wrapOwnedArrayAsIEnumerable` for Array-
// family sources, `wrapOwnedLinkedListAsIEnumerable` for LinkedList
// sources, `drainLazyPipelineToOwnedIEnumerable` for Set/OwningSet
// sources, `wrapUserClassAsIEnumerable` for user-class IOwningEnumerable-
// implementing sources), each of which installs the BORROWING `Current`
// (slot 3) — a function that returns the element pointer at the
// current index WITHOUT transferring ownership out of the underlying
// collection. The IOwningEnumerable<T> spec (§29.1 / §29.11) requires
// slot 3 to be `TakeCurrent` — a function that transfers ownership of
// the pending element AND nulls the pending slot, so the collection's
// slot is empty after each yield (single-consumption drain per §29.11).
// This commit does NOT install that primitive; it eliminates the SEGV
// and observes exact-once disposal only because the for-in desugaring
// auto-disposes each loop variable (`LlvmBackend_Stmts.cpp:7602-7607`)
// and the array's element disposer is null for non-string class
// elements — so the LOOP discharges ownership rather than the slot-3
// TakeCurrent. Re-iterating the same IOwningEnumerable reads the same
// items twice (the array's slots are not nulled), violating single-
// consumption. The TakeCurrent primitive is the class-element follow-
// up #3364.
//
// COVERAGE.
//   (A) BLOCK-BODIED, OwningArray source: `class C { items:
//       IOwningEnumerable<Item> { get => do return C.MakeArr(); end; } }`
//       where `MakeArr` builds a fresh `OwningArray<Item>` and adds three
//       Items. The BLOCK-BODIED return path goes through
//       `wrapAsIEnumerableForTarget` from `emitReturn`. Pre-fix this
//       function's gate keyed on `headName() == "IEnumerable"` and
//       declined the IOwningEnumerable target head, returning the raw
//       OwningArray*; the caller's `for x in c.items` then drove §29.11
//       vtable dispatch on the raw bz_array_t* and read slot 4 (a
//       pointer-sized field — a heap-buffer pointer at offset 32) as a
//       function pointer → SIGSEGV. Post-fix the gate also accepts
//       IOwningEnumerable, and the dispatch at line 7683-7689 routes the
//       OwningArray + callee-owned source to
//       `wrapOwnedArrayAsIEnumerable`, which produces a 5-slot IOwningEnumerable-
//       compatible vtable. The caller's `for x in c.items` reads slot 4
//       (= GetOwningEnumerator) of the wrapper, gets the wrapper's
//       enumerator factory, and iterates. Sum: 35 (5+10+20).
//
//       WHAT THIS TEST DOES NOT PIN. Spec §29.1 / §29.11 requires slot 3
//       to be TakeCurrent — the wrap helper actually installs Current,
//       so the array's slots are not nulled after each yield. A re-
//       iteration over the same wrapper would read the same items twice,
//       violating single-consumption. The follow-up #3364 will install
//       a distinct TakeCurrent primitive that nulls the pending slot on
//       transfer; this test pins the SEGV-fix surface only.
//
// SEMA PERMISSIBILITY. OwningArray<T> implements IOwningEnumerable<T>
// directly (stdlib §4.5), so a property declared `Items:
// IOwningEnumerable<Item>` with body `get => do return new OwningArray<Item>(…);
// end;` compiles at sema — the BLOCK-bodied return coercion accepts
// the OwningArray source as IOwningEnumerable target. The codegen wrap
// (the bug surface) is what was missing.
//
// TEST SOURCE NOTE. The brief identified "non-Array source" as the
// LinkedList / Set / OwningSet / user-class family. Those source types
// do NOT implement IOwningEnumerable<T> at the stdlib type level (the
// stdlib IOwningEnumerable-implementing types are OwningArray<T> and
// the user-defined class family; per stdlib §4.7 OwningLinkedList<T>
// implements IOwningCollection<T> only, not IOwningEnumerable<T>; per
// stdlib §4.10 OwningSet<T> likewise implements IOwningCollection<T>
// only). Their SEMANTIC conversion to IOwningEnumerable<T> is therefore
// REJECTED at §5.1 / §11.6 / §11.2 ("an interface implementing class must
// declare `… : IOwningEnumerable<T>`") — so a property declared at
// IOwningEnumerable<T> whose getter body returns an `OwningLinkedList<T>`
// or `OwningSet<T>` does not compile, and the codegen wrap helper is
// never reached. The wrap helper DOES exist for these source types in
// `wrapAsIEnumerableForTarget` (the LinkedList / Set / user-class arms),
// and would be invoked correctly by the gate fix in this commit if a
// sema-level rule ever admitted those conversions — the wrap helper is
// structurally correct for IEnumerable<T> targets; for IOwningEnumerable<T>
// targets the slot-3 TakeCurrent gap applies (see "WHAT THIS TEST\n// DOES NOT PIN" above) and is the #3364 follow-up.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

constexpr int kExpectedSum = 35;  // 5 + 10 + 20

}  // namespace

// ---------------------------------------------------------------------------
// (A) BLOCK-BODIED getter, OwningArray<Item> source.
//   per §29.2, §23.2 (issue #3355, value-element arm, BLOCK-BODIED cell)
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_getter_value_arm_3355,
           block_bodied_getter_owning_array_no_segv_29_2) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class C "
        "  public property Items: IOwningEnumerable<Item> "
        "    get => do "
        "      var a := new OwningArray<Item>(); "
        "      a.Add(new Item(5)); "
        "      a.Add(new Item(10)); "
        "      a.Add(new Item(20)); "
        "      return a; "
        "    end; "
        "  end Items; "
        "end C; "
        "class Main "
        "  public static method Run(): int "
        "    var total := 0; "
        "    var c := new C(); "
        "    for var x in c.Items do "
        "      using var y := owned x; "
        "      total := total + y.V; "
        "    end for; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);          // no SIGSEGV on slot-4 read
    BEZEL_EXPECT_EQ(r.exitCode, kExpectedSum);
}

// ===========================================================================
// fix #3392 — #3363 structural-remedy item 2 (§23.2 / §28.6 / §28.3 / §29.2,
// v0.282) — the EXPRESSION-BODIED getter's WRAPPER-FRAME LEAK at an
// `IOwningEnumerable<T>` head.
//
// THE BUG. #3355 (the interim fix above) widened the two WRAP-CREATOR gates
// — `wrapAsIEnumerableForTarget` and `wrapPropertyArrayAsIEnumerable`
// (LlvmBackend_Assign.cpp) — from `headName() == "IEnumerable"` to that head
// OR `IOwningEnumerable`, so a property read at an `IOwningEnumerable<T>`
// head now mints a vtable-backed wrapper frame instead of handing back the
// raw backing pointer. It did NOT widen the matching OWNERSHIP-CLAIM
// predicate: `registerIterableSourceDisposal`'s `declIsPureIEnum` in the
// `computedGetterOf` (EXPRESSION-BODIED) arm still spelled the single head
// `tr->headName() == "IEnumerable"`. The local bound to the read therefore
// never claimed the freshly minted frame, and nothing else frees it — the
// field's own holder disposes the BACKING, not the wrap. One frame leaked
// per read.
//
// This is the "duplicated classification predicate drifts from the predicate
// it mirrors" defect class: the wrap site and the ownership-claim site each
// spell the admissible head list independently, so widening one silently
// leaves the other behind.
//
// SPEC. §28.6 (*Leak prevention*) names this exact shape in its
// mint-announcing-typed-field bullet — a stored frame is re-exposed through
// a property, "`public property Items: IEnumerable<Item> => _items;` is the
// stored-view face" — which is §23.2's (*Expression-bodied read-only
// properties*) spelling. Cases (B)–(D) below write the §23.3 block form with
// an EXPRESSION accessor body (`get => _items; end;`) and case (E) writes
// §23.2's shorthand; both spellings reach the same codegen arm
// (`computedGetterOf`, as opposed to `blockGetterOf`'s `get => do … end`)
// and both leaked pre-fix. The claimed frame is discharged by §28.3's scope-end
// auto-drop for the plain `var` that owns it; §29.2 (*Desugaring of `for`*)
// is the vtable protocol the frame implements. `IOwningEnumerable<T>` is
// stdlib §4.13 (v0.282), constrained `where T: class` — hence the class
// element type `Item`.
//
// MEASURED on e9bd762 (this file's parent commit), 200 reads of an
// `IOwningEnumerable<Item>` expression-bodied getter: 6400 bytes leaked in
// 200 allocations under LSan (32 B per wrapper frame). The byte-identical
// program at an `IEnumerable<Item>` head was clean — that pair is the
// discriminator, and case (C) below keeps it. Post-fix both are clean.
//
// SCOPE — WHAT THIS FIX DELIBERATELY DOES NOT TOUCH.
//   * The DECL-SITE arm (`var e: IOwningEnumerable<T> := someOwningArray;`)
//     leaks its frame too, but that leak is NOT IOwningEnumerable-specific:
//     the identical program at an `IEnumerable<T>` head leaks byte-
//     identically (measured: 1600 B in 50 allocations at BOTH heads, 50
//     iterations, i.e. the same 32 B/frame). That is a broader pre-existing
//     defect of the IEnumerable family, filed as #3384 rather than widened
//     into here.
//   * Owning ITERATION semantics over a field-owned source — slot 3's
//     `TakeCurrent` and the §29.11 single-consumption drain — are NOT fixed
//     here. Iterating this shape (`for var x in e do using var y := owned x;`
//     over a field-backed `IOwningEnumerable<T>`) double-disposes every
//     element: the loop discharges it and the field's holder disposes it
//     again. That is an ASan `heap-use-after-free`, measured identically
//     WITH and WITHOUT this fix, and is filed as #3385 (see also #3364).
//     The cases below therefore pin the FRAME's ownership only — they read
//     the property without iterating, so a change in element ownership
//     cannot mask or manufacture the leak they measure.
//   * The unparenthesised `&&`/`||` in `lowerLocalDecl`'s enumerable-local
//     gate (a separate site in the same file, from #1838's 45fa15a, NOT
//     from #3355) is filed as #3386 — it has no known repro, so it does not
//     travel with a fail-without/pass-with fix.
//   * #3363's structural-remedy items 1, 3 and 4 — the
//     `wrapArrayAsIOwningEnumerable` helper with a fresh child enumerator in
//     slot 4, the LinkedList snapshot arm, and the dict-backed Set snapshot
//     arm — are untouched here; #3363 stays open for them.
// ===========================================================================

namespace {

// Shared program body for the leak cases: a stored `owned OwningArray<Item>`
// re-exposed through an EXPRESSION-BODIED getter (§28.6's stored-view face),
// read 200 times. Each read mints one wrapper frame; each must be
// reclaimed at the reading local's scope end. `head` selects the property's
// declared head, `localDecl` the binding spelling under test, and
// `shorthand` picks §23.2's `=> _items;` property spelling — the one §28.6
// writes — over §23.3's block form with an expression accessor body
// (`get => _items; end;`). Both reach the same codegen arm.
std::string storedViewProgram(const std::string& head,
                              const std::string& localDecl,
                              bool shorthand = false) {
    const std::string prop = shorthand
        ? "  public property Items: " + head + "<Item> => _items; "
        : "  public property Items: " + head + "<Item> get => _items; end; ";
    return
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class C "
        "  private owned _items: OwningArray<Item>; "
        "  public constructor() "
        "    _items := new OwningArray<Item>(); "
        "    _items.Add(new Item(9)); "
        "  end constructor; "
        + prop +
        "end C; "
        "class Main "
        "  public static method Run(): int "
        "    using var c := new C(); "
        "    var i := 0; "
        "    while i < 200 do "
        "      " + localDecl + " "
        "      i := i + 1; "
        "    end while; "
        "    return 9; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
}

constexpr int kStoredViewExit = 9;

}  // namespace

// ---------------------------------------------------------------------------
// (B) #3363 CORE: EXPRESSION-BODIED getter at an IOwningEnumerable head, read
//     into an INFERRED local. Pre-fix: 6400 B leaked in 200 allocations.
//   per §23.2, §28.6, §28.3 (issue #3392)
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_getter_value_arm_3355,
           expr_bodied_getter_iowningenumerable_frame_leak_free_28_6) {
    const std::string src =
        storedViewProgram("IOwningEnumerable", "var e := c.Items;");
    JitResult r = runAotChecked(src);
    // runAotChecked runs the generated program under ASan WITH LSan enabled,
    // and fails the run on any "ERROR: LeakSanitizer" / "detected memory
    // leaks" headline — so `ran` is itself the leak assertion here.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kStoredViewExit);
}

// ---------------------------------------------------------------------------
// (C) CONTROL: the byte-identical program at an `IEnumerable<Item>` head was
//     ALREADY leak-free before the fix. It must stay leak-free after it —
//     this is what distinguishes "claim the frame at the owning head" from
//     "move the leak around" or "double-free the borrowed backing".
//   per §23.2, §28.6 (issue #3392)
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_getter_value_arm_3355,
           expr_bodied_getter_ienumerable_twin_stays_leak_free_28_6) {
    const std::string src =
        storedViewProgram("IEnumerable", "var e := c.Items;");
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kStoredViewExit);
}

// ---------------------------------------------------------------------------
// (D) The EXPLICITLY ANNOTATED binding spelling of (B). `declIsPureIEnum`
//     reads `tr` — the annotation when present, else the inferred type — so
//     both spellings route through the widened predicate and both leaked
//     pre-fix. Pinning the annotated form keeps a future narrowing of the
//     predicate to the inference-only path from regressing silently.
//   per §23.2, §28.6, §28.3 (issue #3392)
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_getter_value_arm_3355,
           expr_bodied_getter_iowningenumerable_annotated_local_leak_free_28_6) {
    const std::string src = storedViewProgram(
        "IOwningEnumerable", "var e: IOwningEnumerable<Item> := c.Items;");
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kStoredViewExit);
}

// ---------------------------------------------------------------------------
// (E) §28.6's OWN SPELLING of the stored-view face — the §23.2 expression-
//     bodied property shorthand (`public property Items:
//     IOwningEnumerable<Item> => _items;`) rather than (B)'s §23.3 block
//     form with an expression accessor body. The spec writes the shape this
//     way, so the normative spelling is pinned directly; it reaches the same
//     `computedGetterOf` codegen arm and leaked identically pre-fix.
//   per §23.2, §28.6, §28.3 (issue #3392)
// ---------------------------------------------------------------------------
BEZEL_TEST(iowningenumerable_getter_value_arm_3355,
           expr_bodied_property_shorthand_iowningenumerable_leak_free_28_6) {
    const std::string src = storedViewProgram(
        "IOwningEnumerable", "var e := c.Items;", /*shorthand=*/true);
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kStoredViewExit);
}
