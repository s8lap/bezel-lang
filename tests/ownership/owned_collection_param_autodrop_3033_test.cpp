// Spec-derived regression tests for #3033 — the §28.3 affine auto-drop of an
// `owned` parameter whose type is a stdlib COLLECTION never ran, so the
// container's backing (and, for the owning family, every element it still
// held) leaked.
//
// ── THE RULE ───────────────────────────────────────────────────────────────
// Bezel_Language_Spec.md §28.3 (v0.240), "One mechanism, two specializations,
// one lint", verbatim:
//
//   "Every owned local (and undischarged `owned` parameter) is disposed
//    exactly once on every path by the affine auto-drop above — the
//    exception-path discharge being the same mechanism on the unwinding edge."
//
// and §28.11 "Owning collections", verbatim:
//
//   "The collection's `Dispose()` cascades through every element."
//
// and §28.11 "Borrowing collections", verbatim:
//
//   "The collection's `Dispose()` is a no-op — borrowing collections are
//    trivial-dispose (§28.9), their backing storage part of the instance's
//    single allocation and released with it by the instance's one free"
//
// So for BOTH families the auto-drop of an `owned` collection parameter must
// release the backing; for the owning family it must additionally cascade.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
// `Emitter::registerUnwindOwnedParam` (LlvmBackend_Setup.cpp) gated its whole
// registration on `isClassOrInterfaceRef(rt)`. A collection is a
// compiler-known builtin with NO user TypeSymbol (`rt->declaration ==
// nullptr`), so that gate rejected it and NO disposal entry was pushed at all
// — the identical positive-list-missing shape #1957 had already fixed one
// family out (ChannelSender / Channel / Shared), one allowlist entry at a
// time. Measured on 9c643e7 with the issue's own repro: LSan "184 byte(s)
// leaked in 2 allocation(s)", both frames `bz_array_alloc`.
//
// ── WHAT #3033 GOT WRONG (refuted, recorded) ───────────────────────────────
// The issue reported the leak as "confined to the un-`move`d `using var`
// rebind of an `owned` collection parameter" and asked that the rebind be
// recognised as a discharge. Both halves are wrong:
//
//  (a) The rebind is IRRELEVANT to the leak — `no_rebind_at_all_*` below has
//      no `using var` alias anywhere and leaks identically without the fix.
//      The trigger is the parameter's TYPE, not the body's shape.
//
//  (b) The rebind is NOT a discharge, and must not become one. §28.10
//      "Adopting a named owned binding", verbatim: "An **unmarked** `using
//      var y := x;` where `x` is a named owned binding does *not* adopt: it
//      is a borrow, pinned per the subsection below, and `x` retains the
//      obligation." So the diagnostic is RIGHT TO FIRE on the issue's repro;
//      what was wrong is what it SAID — its remedy list named the unmarked
//      rebind, a spelling that discharges nothing. That text now names the
//      marker (`using var y := owned x;`), pinned by
//      `diagnostic_names_the_owned_marker_spec_28_10` below.
//
// ── OBSERVABILITY ──────────────────────────────────────────────────────────
// Every accept case uses `runAotChecked` (per-program ASan/LSan, per
// tests/README.md): without the fix each is an LSan DIRECT leak of the
// backing, which flips `r.ran` to false. The exit code additionally pins that
// the program computed the right answer, so a vacuous "it exited 0" cannot
// certify the path.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// A non-trivial-dispose element (§28.9: it has a destructor), so an
// OwningArray<Cell> auto-drop must CASCADE into it — the destructor's marker
// is how the cascade is observed rather than merely inferred from LSan.
const char* kCell =
    "class Cell "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor Console.WriteLine($\"D{V}\"); end destructor; "
    "end Cell; ";

}  // namespace

// ===========================================================================
// (1) The issue's OWN repro — `using var src := elements;` over an
//     `owned OwningArray<Cell>` parameter, drained to empty.
// ===========================================================================

// #3033's verbatim repro shape. The drain empties the array, so ONLY the
// bz_array_t backing (header + slot storage) is left for the auto-drop —
// exactly the 184 bytes the issue measured. Exit 6 = 1+2+3.
BEZEL_TEST(owned_collection_param_autodrop_3033,
           issue_repro_using_var_rebind_of_owned_array_param_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class Bag "
        "  public Total: int; "
        "  public constructor(t: int) Total := t; end constructor; "
        "  public static method Consume(elements: owned OwningArray<Cell>): owned Bag "
        "    using var src := elements; "
        "    var s := 0; "
        "    while src.Count > 0 do "
        "      using var c := src.PopFirst(); "
        "      s := s + c.V; "
        "    end; "
        "    return new Bag(s); "
        "  end Consume; "
        "end Bag; "
        "class C "
        "  public static method Main(): int "
        "    var arr := new OwningArray<Cell>(); "
        "    arr.Add(new Cell(1)); "
        "    arr.Add(new Cell(2)); "
        "    arr.Add(new Cell(3)); "
        "    using var bg := Bag.Consume(owned arr); "
        "    return bg.Total; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    // WITHOUT the fix: LSan "184 byte(s) leaked in 2 allocation(s)" (the
    // bz_array_t header + its slot storage) → r.ran false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
    // The drain + each `using var c` disposed every element exactly once; the
    // auto-drop's cascade over the now-empty array adds no second disposal.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D3"), 1);
}

// REFUTES #3033's stated scope: no `using var` rebind anywhere in the method,
// same leak. The trigger is the parameter's TYPE, not the rebind — so a fix
// aimed at the rebind (as the issue proposed) would have left this leaking.
BEZEL_TEST(owned_collection_param_autodrop_3033,
           no_rebind_at_all_owned_array_param_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Consume(elements: owned OwningArray<Cell>): int "
        "    var s := 0; "
        "    while elements.Count > 0 do "
        "      using var c := elements.PopFirst(); "
        "      s := s + c.V; "
        "    end; "
        "    return s; "
        "  end Consume; "
        "  public static method Main(): int "
        "    var arr := new OwningArray<Cell>(); "
        "    arr.Add(new Cell(1)); "
        "    arr.Add(new Cell(2)); "
        "    arr.Add(new Cell(3)); "
        "    return C.Consume(owned arr); "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// The CASCADE, not just the backing: the parameter is never drained, so the
// auto-drop must dispose the two live elements AND free the backing. §28.11:
// "The collection's `Dispose()` cascades through every element."
BEZEL_TEST(owned_collection_param_autodrop_3033,
           undrained_owning_array_param_cascades_to_elements_spec_28_11) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Consume(elements: owned OwningArray<Cell>): int "
        "    return elements.Count + 40; "
        "  end Consume; "
        "  public static method Main(): int "
        "    var arr := new OwningArray<Cell>(); "
        "    arr.Add(new Cell(1)); "
        "    arr.Add(new Cell(2)); "
        "    return C.Consume(owned arr); "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    // WITHOUT the fix: LSan leaks the backing AND both Cell instances.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    // Each element disposed EXACTLY once by the cascade (not zero — the leak;
    // not twice — a double-free the cascade must not introduce).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}

// ===========================================================================
// (2) AXIS: the collection FAMILY. `registerUnwindOwnedParam` now routes
//     through `buildCollectionTempDisposal`, whose three arms select
//     bz_llist_free / bz_dict_free / bz_array_free. One cell per arm — a fix
//     that wired up only the array-backed arm would leave two leaking.
// ===========================================================================

// bz_llist_free arm (§4.7): OwningLinkedList<T> is bz_llist_t-backed, NOT
// bz_array_t-backed, and must cascade into its nodes' values.
BEZEL_TEST(owned_collection_param_autodrop_3033,
           owned_linked_list_param_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Take(xs: owned OwningLinkedList<Cell>): int "
        "    return xs.Count + 40; "
        "  end Take; "
        "  public static method Main(): int "
        "    var xs := new OwningLinkedList<Cell>(); "
        "    xs.AddLast(new Cell(1)); "
        "    xs.AddLast(new Cell(2)); "
        "    return C.Take(owned xs); "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}

// bz_dict_free arm (§4.9): OwningDictionary<K,V> owns its VALUES (not its
// keys, §28.11) — the value-cascade disposer must run.
BEZEL_TEST(owned_collection_param_autodrop_3033,
           owned_dictionary_param_cascades_values_spec_28_11) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Take(d: owned OwningDictionary<string, Cell>): int "
        "    return d.Count + 40; "
        "  end Take; "
        "  public static method Main(): int "
        "    var d := new OwningDictionary<string, Cell>(); "
        "    d.Add(\"a\", new Cell(1)); "
        "    d.Add(\"b\", new Cell(2)); "
        "    return C.Take(owned d); "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}

// bz_dict_free arm via the DICT-BACKED Set (#1012 / §4.10): a `Set<int>` is
// hash-backed, not array-backed, so the wrong arm would leak the bz_dict
// struct rather than merely mis-free it.
BEZEL_TEST(owned_collection_param_autodrop_3033,
           owned_dict_backed_set_param_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Take(s: owned Set<int>): int "
        "    return s.Count + 40; "
        "  end Take; "
        "  public static method Main(): int "
        "    var s := new Set<int>(); "
        "    s.Add(1); "
        "    s.Add(2); "
        "    return C.Take(owned s); "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (3) AXIS: the FAMILY (borrowing vs owning) and the ELEMENT policy. A
//     borrowing collection disposes no element but still owns its backing
//     (§28.11: "released with it by the instance's one free"), and a
//     borrowing `Array<string>` additionally co-owns a +1 per element that
//     the free must release (§28.9 / §4.6).
// ===========================================================================

// BORROWING family, value elements — backing only, no cascade to run.
BEZEL_TEST(owned_collection_param_autodrop_3033,
           owned_borrowing_array_param_frees_backing_spec_28_11) {
    JitResult r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Take(a: owned Array<int>): int "
        "    return a.Count + 40; "
        "  end Take; "
        "  public static method Main(): int "
        "    var a := new Array<int>(); "
        "    a.Add(5); "
        "    a.Add(6); "
        "    return C.Take(owned a); "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    // WITHOUT the fix: LSan "120 byte(s) leaked in 2 allocation(s)".
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// BORROWING family, runtime-managed (string) elements: the array co-owns a +1
// per element, so the auto-drop's free must carry the string-release element
// disposer or each element's heap data leaks alongside the backing.
BEZEL_TEST(owned_collection_param_autodrop_3033,
           owned_borrowing_string_array_param_releases_elements_spec_28_9) {
    JitResult r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Take(a: owned Array<string>): int "
        "    return a.Count + 40; "
        "  end Take; "
        "  public static method Main(): int "
        "    var a := new Array<string>(); "
        "    a.Add(\"alpha\"); "
        "    a.Add(\"beta\"); "
        "    return C.Take(owned a); "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// (4) AXIS: the EXIT EDGE. The registration is on `disposeStack_.front()`
//     with no unwind-only gate, so it must fire on the unwinding edge too and
//     exactly once there — §28.3: "the exception-path discharge being the
//     same mechanism on the unwinding edge."
// ===========================================================================

BEZEL_TEST(owned_collection_param_autodrop_3033,
           owned_array_param_disposed_on_unwind_edge_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Take(a: owned OwningArray<Cell>): int "
        "    if a.Count > 1 then throw new InvalidOperationException(\"boom\"); end; "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell(1)); "
        "    a.Add(new Cell(2)); "
        "    try "
        "      return C.Take(owned a); "
        "    catch e: InvalidOperationException => "
        "      return 43; "
        "    end; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    // WITHOUT the fix: the backing and both elements leak out the throw edge.
    // A double-registration would instead be an ASan heap-use-after-free here.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 43);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}

// ===========================================================================
// (5) AXIS: DISCHARGED, not auto-dropped. The new branch is gated on
//     `ps->autoDropAtScopeEnd`; a parameter the body moves out or disposes
//     must get NO registration, or the normal exit double-frees. This is the
//     cell that fails loudly (ASan heap-use-after-free) if the gate is
//     dropped — the strongest attack on the fix, pinned.
// ===========================================================================

BEZEL_TEST(owned_collection_param_autodrop_3033,
           discharged_owned_array_param_not_double_freed_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        // Discharge by RETURN (ownership transfers to the caller's `using var`).
        "  public static method Fwd(a: owned OwningArray<Cell>): owned OwningArray<Cell> "
        "    return a; "
        "  end Fwd; "
        // Discharge by explicit `.Dispose()`.
        "  public static method Disp(a: owned OwningArray<Cell>): int "
        "    var n := a.Count; "
        "    a.Dispose(); "
        "    return n; "
        "  end Disp; "
        "  public static method Main(): int "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell(1)); "
        "    using var b := C.Fwd(owned a); "
        "    var c2 := new OwningArray<Cell>(); "
        "    c2.Add(new Cell(8)); "
        "    c2.Add(new Cell(9)); "
        "    return C.Disp(owned c2) + b.Count + 41; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    // A registration on the discharged path would be an ASan
    // heap-use-after-free / double-free here → r.ran false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);  // Disp=2 + b.Count=1 + 41
    // Every element disposed exactly once across both discharge spellings.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D8"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D9"), 1);
}

// ===========================================================================
// (6) AXIS: NULLABILITY. `owned OwningArray<T>?` bound to null — the free
//     must be a no-op, not a null deref (§28.9: disposal of null is a no-op).
// ===========================================================================

BEZEL_TEST(owned_collection_param_autodrop_3033,
           owned_nullable_array_param_null_is_noop_spec_28_9) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Take(a: owned OwningArray<Cell>?): int "
        "    if a = null then return 41; end; "
        "    return 42; "
        "  end Take; "
        "  public static method Main(): int "
        "    return C.Take(null); "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (7) The DIAGNOSTIC half of #3033 — right to fire, wrong in what it said.
// ===========================================================================

// The auto-drop warning must still FIRE on the issue's repro: per §28.10 the
// unmarked `using var src := elements;` is a borrow-pin, so `elements` really
// does retain the obligation. #3033 asked for this warning to be silenced;
// that request is REFUTED by the spec and this cell pins the refusal.
BEZEL_TEST(owned_collection_param_autodrop_3033,
           unmarked_using_var_rebind_still_warns_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C "
        "  public static method Consume(elements: owned OwningArray<Cell>): int "
        "    using var src := elements; "
        "    return src.Count; "
        "  end Consume; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    bool warned = false;
    for (const auto& d : r.diagnostics.diagnostics())
        // narrow #4743: the second conjunct read `"§28.3 auto-drop"`, which stopped
        // occurring in the message when fix #4735 (af645679, this same batch)
        // renamed it to `"§28.3 must-use / auto-drop"` to match the spec's own
        // name for the lint. #4735 did not update this needle, and the omission
        // was invisible because #4743 (03c1c2bd, one hour earlier in the same
        // batch) had already suppressed the warning here outright — the row was
        // red for the other reason, so the stale needle never got its own
        // failure. Both defects have to be fixed for the row to go green: the
        // §28.9 gate narrowing restores the warning, and this needle now spells
        // the current name. Pinning the FULL current phrase rather than the
        // shorter `"auto-drop"` keeps this a rename detector.
        if (d.message.find("owned parameter `elements`") != std::string::npos &&
            d.message.find("§28.3 must-use / auto-drop") != std::string::npos)
            warned = true;
    BEZEL_EXPECT_TRUE(warned);
}

// ...and its remedy list must name the MARKED adoption. §28.10 verbatim: "An
// **unmarked** `using var y := x;` where `x` is a named owned binding does
// *not* adopt". The old text said "rebind it with `using var`" — advice that
// discharges nothing, which is how #3033's author reached a leak by following
// the compiler's own suggestion. Asserting the TEXT, not merely that a
// diagnostic fired.
BEZEL_TEST(owned_collection_param_autodrop_3033,
           diagnostic_names_the_owned_marker_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C "
        "  public static method Consume(elements: owned OwningArray<Cell>): int "
        "    return elements.Count; "
        "  end Consume; "
        "end C;");
    std::string msg;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("owned parameter `elements`") != std::string::npos)
            msg = d.message;
    BEZEL_EXPECT_TRUE(!msg.empty());
    // The remedy names the marked adoption, spelled with this parameter's name.
    BEZEL_EXPECT_TRUE(msg.find("using var elements2 := move elements;") !=
                      std::string::npos);
    // ...and says why the marker is required, citing §28.10.
    BEZEL_EXPECT_TRUE(msg.find("borrow-pin") != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("§28.10") != std::string::npos);
    // The three other §28.3 remedies survive.
    BEZEL_EXPECT_TRUE(msg.find("`move`") != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("`return`") != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("`.Dispose()`") != std::string::npos);
}

// The remedy the diagnostic now names must actually WORK — the marked
// adoption discharges the parameter, no warning, no leak. §28.10: "`using var
// y := owned x;` transfers a named owned local or `owned` parameter into a
// scope-bound binding: `y` takes over the disposal obligation".
BEZEL_TEST(owned_collection_param_autodrop_3033,
           marked_owned_adoption_discharges_and_is_leak_free_spec_28_10) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Take(a: owned OwningArray<Cell>): int "
        "    using var a2 := owned a; "
        "    return a2.Count + 40; "
        "  end Take; "
        "  public static method Main(): int "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell(1)); "
        "    a.Add(new Cell(2)); "
        "    return C.Take(owned a); "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}
