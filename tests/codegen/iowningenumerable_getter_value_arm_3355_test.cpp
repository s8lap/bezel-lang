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

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

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
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // no SIGSEGV on slot-4 read
    BEZEL_EXPECT_EQ(r.exitCode, kExpectedSum);
}