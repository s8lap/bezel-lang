// Codegen / wild jump — fix #3519 (§29.2 / §28.9 / §28.3, v0.283; stdlib §4.8 /
// §4.17): the `Dictionary` family had NO ARM AT ALL in the §29.2 enumerable-wrap
// grid, so a `Dictionary<K, V>` coerced to `IEnumerable<DictionaryEntry<K, V>>`
// was handed to the consumer as a raw `bz_dict_t*`.
//
// THE DEFECT. `wrapAsIEnumerableForTarget` (LlvmBackend_Assign.cpp) dispatches
// on the SOURCE family: array-enumerator sources, class-element `Set`,
// dict-backed `Set`, `LinkedList`, user class — then falls off the end to a
// bare `return v`. `Dictionary` / `OwningDictionary` / `SortedDictionary` hit
// that fall-through. §29.2's desugaring then drives the value through the
// enumerator vtable (`__source.GetEnumerator()` is slot 4), so the first word
// of a `bz_dict_t` is called as a function pointer.
//
// The local-decl mint in `LlvmBackend_Setup.cpp` — the SAME decision at its
// other face, which #3419 exists to keep in step — had the identical hole, and
// so did the argument coercion (which shares the Assign.cpp function). MEASURED
// on `e57a1eb`, all three deterministic:
//
//   RETURN      `return d;` at an `IEnumerable<DictionaryEntry<int,int>>` head
//               -> SIGSEGV, rc 139
//   LOCAL-DECL  `var e: IEnumerable<DictionaryEntry<int,int>> := d;`
//               -> SIGSEGV, rc 139
//   ARGUMENT    `Sum(d)` where the parameter is that head
//               -> SIGSEGV, rc 139
//   IN PLACE    `for var x in d do` (no coercion)  -> n=30, rc 0
//
// Under ASan the fault is a WILD JUMP — `pc` equal to the faulting address in a
// non-executable region ("Maybe a wild jump?") — which is what an indirect call
// through a data word looks like. The tell in the object file is that the
// failing program emits no `bz$…enum…$vtable` pair at all: no wrap was minted,
// so there was nothing to jump to.
//
// ★ THE OWNERSHIP AXIS HAD ALREADY BEEN WIDENED. This is #3419's shape in its
// most legible form: `returnSourceIsCalleeOwnedCollection` has named
// `Dictionary`, `OwningDictionary` and `SortedDictionary` since #2299 — it must,
// or the source would be double-freed — so `srcOwnedByCallee` arrived at the
// wrap function TRUE for precisely the family the wrap function could not
// handle. One axis of the grid knew about dictionaries and the other did not.
//
// THE FIX — NO NEW PREDICATE, NO NEW WRAP HELPER. Both authorities already
// existed and were already exercised by the explicit `.GetEnumerator()` face:
// `wrapDictionaryAsIEnumerable` snapshots the live keys+values, decodes one
// `{K, V}` entry struct per pair and returns an OWNING array enumerator over
// that snapshot (routing the string-key shape through
// `wrapDictionaryEntryArrayAsIEnumerable` so the rebuilt ref-counted keys are
// released), and `wrapSortedDictionaryAsIEnumerable` is its ordered sibling.
// The classifier is `isDictTypeName`, the emitter's existing face of
// `common::isDictionaryName` — the same authority the indexer, `.Count` and the
// in-place for-in already read. Both faces now call them.
//
// ORDERING IS PART OF THE FIX, NOT A BONUS. A `SortedDictionary` must take the
// sorted sibling: the plain snapshot is in hash order, and stdlib §4.17
// ("iteration visits entries in ascending key order") would be silently broken
// by it — the same contract reason `SortedSet` is excluded from the array arm.
// Row (E) is that assertion; a fix that wrapped every dictionary with the plain
// helper passes every other row in this file.
//
// OWNERSHIP. The snapshot is INDEPENDENT of the source dictionary (fresh
// `bz_array_t`, rebuilt keys, copied value words), so the wrapper's slot-1
// Dispose reclaims exactly the snapshot and never the source. A callee-OWNED
// source is therefore freed at the coercion (`freeDictSourceNow`, #2299's
// treatment of the dict-backed `Set` arm) — scope-end disposal is not an
// option, it would race the caller's iteration.
//
// ⚠ UPDATED BY fix #3568 (v0.286). This header used to say flatly that
// "`bz_dict_free(d, null)` leaves the values untouched, which is what keeps the
// snapshot's borrowed value words live for that iteration". The first half was
// the DEFECT — a real dictionary's values were then never disposed or released,
// which for the `string` VALUE of row (I) leaked 14580 B in 800 allocations as
// soon as the values were built rather than literal. The second half is still
// exactly right, and is why the fix could not simply pass a non-null disposer
// here: `Emitter::dictSnapshotValueDisposers` splits the obligation, and only
// the half the snapshot COPIED OUT (a record / nullable-value box) is
// discharged at this free. The half the snapshot ALIASED moves to the wrapper's
// slot-1 Dispose. See `dict_snapshot_value_disposer_3568_test.cpp`.
// At the local-decl face the free is gated on the
// initializer being a `new` EXPRESSION, the same alias-rejection gate the
// Array / Set / LinkedList arms there use: a producer-init IDENTIFIER source is
// still owned by that identifier and freeing it here would DOUBLE-FREE.
//
// ORACLE. `runAotChecked` compiles the generated program with AddressSanitizer
// + LeakSanitizer, so `r.ran` is simultaneously a no-fault, no-double-free and
// no-leak assertion. Row (A) is deliberately the PLAIN build as well (#2633:
// neither oracle dominates), and the leak rows are loop-amplified 200x because
// a one-shot LSan row can pass on both sides of a fix while the allocation is
// still reachable at exit.
//
// ★ WHAT THIS ARM DELIBERATELY DOES **NOT** CLAIM (narrowed on review).
// As of v0.283 the arm admitted only `DictionaryEntry<K, V>` where BOTH K and
// V are a NON-NULLABLE PRIMITIVE — the scalars, `string`, and `Object`. Every
// other entry shape (class-, record- or enum-typed K or V, and any NULLABLE
// one) fell through to the historical `return v` and kept SIGSEGVing exactly as
// it did before that commit.
//
// ⚠ UPDATED BY fix #3568 (v0.286): a NULLABLE primitive V, an ENUM V and an
// ENUM K are now ADMITTED — the reason they were declined was the missing
// value disposer, and that is fixed. A record-, class- or interface-typed V is
// still declined, but on issue 3563 alone now (the read side), not on this
// file's disposal argument. The rows below are unchanged and unaffected; the
// widened shapes are covered by
// `tests/codegen/dict_snapshot_value_disposer_3568_test.cpp`.
//
// ⚠ Two boundaries worth stating, because an earlier revision of this header
// and of the gate's own comment got both wrong in the SAME direction — they
// described declines the code did not perform:
//   * `Object` is ADMITTED, not declined. `PrimitiveType::Object` is a
//     PrimitiveType ("§4 Object class — treated as a primitive lookup key"),
//     so `Dictionary<int, Object>` passes the gate. Measured `iters=200`,
//     rc 0, LSan clean at 200x — an `Object` value is a borrowed reference the
//     table does not own, so the null value disposer is sound for it exactly
//     as for `string`.
//   * NULLABILITY IS NOT A `Kind`. There is no `Kind::Nullable`; `nullable` is
//     a separate `bool` on the same TypeRef, so `int?` is `Kind::Primitive`
//     and a kind-only gate ADMITS it. Measured on the kind-only build,
//     `Dictionary<int, int?>` at 200x leaked 3200 B in 400 allocations under
//     LSan — issue 3568's null-value-disposer leak through a second door. At
//     v0.283 the gate tested `->nullable` explicitly and that shape went back
//     on the historical path (rc 139), which is why no row in THIS file
//     exercises it. ⚠ fix #3568 (v0.286) ADMITS it, with the box now freed at
//     the source free; row (D) of
//     `dict_snapshot_value_disposer_3568_test.cpp` is the pin. The remaining
//     declines are the record / class / interface VALUE shapes. That is deliberate, and it is the point: measured on the
// pre-narrowing build, a `Dictionary<int, Item>` at this head returned
// `t=0` where the in-place control gives `t=10`, and a
// `Dictionary<int, P>` (record value) leaked 800 B in 200 objects at 200
// iterations while an `OwningDictionary<int, Item>` leaked 3200 B in 200 —
// exactly its owned values. **A crash converted into a silent wrong answer
// is the worse outcome**, so the arm claims only what it carries end to end.
//
// Two independent defects sit behind that exclusion, both filed:
//   * issue 3563 — `x.Value.<member>` on the WRAPPED entry face fails to
//     lower (the #2961 internal, store dropped -> the wrong answer). It
//     depends only on `x`'s static type, so it is pre-existing; it was
//     unreachable behind the SIGSEGV and would have become reachable here.
//   * issue 3568 — `freeDictSourceNow` passed a NULL value disposer
//     (`bz_dict_free(d, null)`). It was written for the dict-backed `Set`
//     arm, where the table has keys and no values; on a real dictionary the
//     values were never disposed or released, which is the leak above.
//     ⚠ FIXED in v0.286; see `dict_snapshot_value_disposer_3568_test.cpp`.
// Widening the remaining shapes is now blocked on 3563 alone. Issue 3568 also
// records the adjacent sema question — stdlib §4.9 declares
// `OwningDictionary` `: IDisposable` only and says entry iteration is *not
// provided* on it, yet the coercion type-checks today.
//
// Verified negative on the narrowing itself: rows (F) and (I) pin the two
// supported edges — a `string` KEY and a `string` VALUE — so a future
// narrowing that over-tightens to "scalars only" fails loudly here.
// ⚠ Both use string LITERALS, whose release is a no-op on a sentinel; that is
// precisely why neither caught issue 3568's leak on the value axis, which only
// appears once the values are BUILT at run time.
//
// COVERAGE.
//   (A) The issue's repro at the RETURN face, PLAIN build.
//   (B) The same program SANITIZED.
//   (C) The LOCAL-DECL face — the sibling mint, broken identically.
//   (D) The ARGUMENT face — same function, borrowed source (no free).
//   (E) `SortedDictionary` through the RETURN face: ascending key order.
//   (F) String keys, 200x under LSan — the rebuilt `bz_string` keys are
//       released by the entry-array wrapper, and the source dict is freed
//       exactly once.
//   (G) An unbound `new Dictionary` at a LOCAL-DECL head, 200x under LSan —
//       the `new`-gated `freeDictSourceNow` at that face.
//   (H) Verified negative: in-place iteration of the same dictionary, which
//       takes no coercion at all and was already clean.
//   (I) A `string` VALUE, 200x under LSan — the supported edge on the value
//       axis, and the row that fails if the shape gate over-tightens.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 8 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 8 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is still recognised as an environment condition, but it now MARKS THE
// ROW FAILED instead of hiding it. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The issue's own repro, verbatim in shape. `d` is a producer-init local, so
// `returnSourceIsCalleeOwnedCollection` classifies the return source as
// callee-owned and the coercion takes the owning arm.
// (Never put a C++ `//` comment inside these literals: it is lexed as Bezel
// source and comments out the rest of the program. Bezel's own comment forms
// are `//`, `/* ... */` and `///` -- spec §1.5, which is exhaustive.)
const char* const kReturnRepro =
    "module M; "
    "public class Main "
    "  public static method Mk(): IEnumerable<DictionaryEntry<int, int>> "
    "    var d := new Dictionary<int, int>(); "
    "    d[1] := 10; "
    "    d[2] := 20; "
    "    return d; "
    "  end Mk; "
    "  public static method Main(): int "
    "    using var e := Mk(); "
    "    var n := 0; "
    "    for var x in e do n := n + x.Value; end for; "
    "    Console.WriteLine($\"n={n}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

}  // namespace

// ---------------------------------------------------------------------------
// (A) The pin at the RETURN face, PLAIN build.
//   per §29.2 "Desugaring of `for`" (v0.283): "using var __enumerator :=
//   __source.GetEnumerator();" — the coerced value must BE an enumerator.
//   stdlib §4.8: iteration yields DictionaryEntry<K,V> in insertion order.
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_dictionary_ienumerable_wrap_3519,
           callee_owned_dictionary_return_iterates_its_entries_29_2) {
    JitResult r = runAot(kReturnRepro);
    // Pre-fix on e57a1eb: SIGSEGV, rc 139 — `r.ran` is WIFEXITED, so false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=30"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (B) The SAME program, SANITIZED — the wild jump, and the disposal claim.
//   per §28.9 (the RETURN coercion), §28.2 (dispose exactly once)
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_dictionary_ienumerable_wrap_3519,
           callee_owned_dictionary_return_is_not_a_wild_jump_28_9) {
    JitResult r = runAotChecked(kReturnRepro);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix: "SEGV on unknown address 0x… (pc 0x… ) … Hint: PC is at a
    // non-executable region. Maybe a wild jump?". `r.ran` is also the no-leak
    // half: the snapshot is freed by the wrapper's slot-1 Dispose and the
    // source dictionary by the coercion's own freeDictSourceNow, once each.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=30"));
}

// ---------------------------------------------------------------------------
// (C) The LOCAL-DECL face — the sibling mint in LlvmBackend_Setup.cpp.
//   per §29.2, §28.3 "Local variables, ownership, and borrows"
//
// The source is a NAMED local, so the local-decl arm's `new`-gate declines the
// source free: `d` still owns the dictionary and disposes it at its own scope
// end. `r.ran` under LSan is the no-double-free assertion for that gate.
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_dictionary_ienumerable_wrap_3519,
           dictionary_bound_to_an_ienumerable_local_is_wrapped_29_2) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Main(): int "
        "    var d := new Dictionary<int, int>(); "
        "    d[1] := 10; "
        "    d[2] := 20; "
        "    var e: IEnumerable<DictionaryEntry<int, int>> := d; "
        "    var n := 0; "
        "    for var x in e do n := n + x.Value; end for; "
        "    Console.WriteLine($\"n={n}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix on e57a1eb: SIGSEGV, rc 139 — identical symptom to (A)/(B).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=30"));
}

// ---------------------------------------------------------------------------
// (D) The ARGUMENT face — the third position `wrapAsIEnumerableForTarget`
//     serves. The source is a BORROWED named local, so no source free fires;
//     the wrapper frame + snapshot are reclaimed at the call's scope end.
//   per §29.2, §28.3
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_dictionary_ienumerable_wrap_3519,
           dictionary_passed_as_an_ienumerable_argument_is_wrapped_29_2) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Sum(e: IEnumerable<DictionaryEntry<int, int>>): int "
        "    var n := 0; "
        "    for var x in e do n := n + x.Value; end for; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    var d := new Dictionary<int, int>(); "
        "    d[1] := 10; "
        "    d[2] := 20; "
        "    Console.WriteLine($\"n={Sum(d)}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix on e57a1eb: SIGSEGV, rc 139.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=30"));
}

// ---------------------------------------------------------------------------
// (E) `SortedDictionary` through the RETURN face keeps ASCENDING KEY ORDER.
//   per stdlib §4.17 (v0.283): "A comparison-ordered associative collection:
//   iteration visits entries in ascending key order."
//
// This is the row that separates the CORRECT fix from the tempting one. The
// keys are inserted 3, 1, 2; the plain `wrapDictionaryAsIEnumerable` snapshot
// is in the dictionary's own storage order, so a fix that routed every
// dictionary through it would silently yield an unordered sequence here while
// passing every other row in this file.
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_dictionary_ienumerable_wrap_3519,
           sorted_dictionary_return_keeps_ascending_key_order_4_17) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Mk(): IEnumerable<DictionaryEntry<int, int>> "
        "    var d := new SortedDictionary<int, int>(); "
        "    d[3] := 3; "
        "    d[1] := 1; "
        "    d[2] := 2; "
        "    return d; "
        "  end Mk; "
        "  public static method Main(): int "
        "    using var e := Mk(); "
        "    var order := 0; "
        "    for var x in e do order := order * 10 + x.Key; end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix on e57a1eb: SIGSEGV, rc 139.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=123"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "order=312"));
}

// ---------------------------------------------------------------------------
// (F) STRING KEYS, loop-amplified 200x under LSan.
//   per stdlib §4.8, §28.9, §28.2
//
// The string-key materialisation REBUILDS each key as a fresh ref-counted
// `bz_string` (rc=1), which the plain owning array wrapper's null element
// disposer cannot reach — `wrapDictionaryEntryArrayAsIEnumerable` is the
// string-key sibling that walks the entry structs and releases offset 0. The
// keys are LITERALS here only for the key side; the amplification is what makes
// the row able to fail at all, since a single iteration's allocations can still
// be reachable at exit.
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_dictionary_ienumerable_wrap_3519,
           string_keyed_dictionary_return_releases_its_rebuilt_keys_28_11) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Mk(): IEnumerable<DictionaryEntry<string, int>> "
        "    var d := new Dictionary<string, int>(); "
        "    d[\"a\"] := 1; "
        "    d[\"bb\"] := 2; "
        "    return d; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e := Mk(); "
        "      for var x in e do total := total + x.Key.Length + x.Value; end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x ((1 + 1) + (2 + 2)) == 1200.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1200"));
}

// ---------------------------------------------------------------------------
// (G) An UNBOUND `new Dictionary` at a LOCAL-DECL head, 200x under LSan.
//   per §28.3, §29.2
//
// This is the arm's OWNING gate: a `new` at a local-decl site is bound to no
// other name by construction, so the coercion frees the source dictionary
// itself (the snapshot is independent). Without that free the row leaks one
// `bz_dict_t` per iteration; with a free applied to the NAMED-source shape of
// row (C) instead, that row double-frees. Both halves are pinned.
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_dictionary_ienumerable_wrap_3519,
           unbound_new_dictionary_at_an_ienumerable_local_is_freed_once_28_3) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e: IEnumerable<DictionaryEntry<int, int>> := "
        "          new Dictionary<int, int>(); "
        "      for var x in e do total := total + x.Value; end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix on e57a1eb this SEGV'd like every other coerced shape; the
    // dictionary is empty by construction, so the value observable is 0 and
    // `r.ran` (no fault, no leak, no double free) carries the row.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=0"));
}

// ---------------------------------------------------------------------------
// (H) Verified negative — IN-PLACE iteration takes no coercion and is
//     unchanged by this fix. This is the control the issue reported clean.
//   per stdlib §4.8 (entry iteration in insertion order)
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_dictionary_ienumerable_wrap_3519,
           in_place_dictionary_iteration_is_unchanged_4_8) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Main(): int "
        "    var d := new Dictionary<int, int>(); "
        "    d[1] := 10; "
        "    d[2] := 20; "
        "    var n := 0; "
        "    for var x in d do n := n + x.Value; end for; "
        "    Console.WriteLine($\"n={n}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Identical on both sides of the fix.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=30"));
}

// ---------------------------------------------------------------------------
// (I) A `string` VALUE at the RETURN face, 200x under LSan — the supported
//     edge on the VALUE axis, mirroring row (F) on the key axis.
//   per stdlib §4.8, §28.9
//
// `string` is a PRIMITIVE, so it is inside the shape gate; a class or record
// value is not, and is excluded for the reasons in the header. This row is
// what makes the gate's boundary testable in the admitting direction: a
// narrowing that tightened to "scalars only" would leave the row red rather
// than silently shrinking the fix.
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_dictionary_ienumerable_wrap_3519,
           string_valued_dictionary_return_is_inside_the_shape_gate_4_8) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Mk(): IEnumerable<DictionaryEntry<int, string>> "
        "    var d := new Dictionary<int, string>(); "
        "    d[1] := \"aa\"; "
        "    d[2] := \"bbb\"; "
        "    return d; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e := Mk(); "
        "      for var x in e do total := total + x.Value.Length; end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x (2 + 3) == 1000.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}
