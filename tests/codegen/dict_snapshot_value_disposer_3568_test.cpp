// Codegen / leak — fix #3568 (§29.2 / §28.9 / §28.11; stdlib §4.8, v0.286):
// `freeDictSourceNow` freed a callee-owned dictionary source with a NULL value
// disposer, so every heap-allocated VALUE in the table was neither disposed nor
// released.
//
// THE DEFECT. `bz_dict_free(d, dispose_val)` frees each key copy unconditionally
// and calls `dispose_val(e.val)` on each live value only if `dispose_val` is
// non-null. `freeDictSourceNow` hardcoded null. That is exactly right for the
// only caller it was written for — the dict-backed `Set<T>` arm of
// `wrapAsIEnumerableForTarget`, whose hash table has keys and no values — and
// wrong for a real dictionary. #3519 then routed the `Dictionary` family through
// the same helper, so the shipped v0.283 arm leaked every value the table owned.
//
// MEASURED on `5d052b3` (the parent of this commit), 200 iterations under
// ASan/LSan, a `Dictionary<int, string>` returned at an
// `IEnumerable<DictionaryEntry<int, string>>` head with INTERPOLATED (heap)
// values:
//
//     total=1980, rc 1
//     SUMMARY: AddressSanitizer: 14580 byte(s) leaked in 800 allocation(s).
//
// — 400 built strings (two per iteration), each a header plus its data buffer.
// The same program with STRING LITERAL values is clean on both sides of the fix,
// because a literal's release is a no-op on a sentinel: that is why the shipped
// arm's own row (I) in `callee_owned_dictionary_ienumerable_wrap_3519_test.cpp`
// did not catch this. Row (B) below is that literal control, kept so the fix
// cannot turn into an over-release on the sentinel path.
//
// ★ WHY A NON-NULL DISPOSER AT THE SOURCE FREE IS THE WRONG FIX, AND THE SHAPE
// OF THE RIGHT ONE. The snapshot `wrapDictionaryAsIEnumerable` materialises does
// two different things with a value slot, and which one it did decides where the
// disposal has to happen:
//
//   * it LOADS THE VALUE OUT of a heap box — a record value (`denum.vrec`) and a
//     nullable value type (`denum.vnv`) are both `CreateLoad`ed by value — so the
//     snapshot holds its own bytes and the box is dead the instant the snapshot
//     exists. That box must be freed AT THE SOURCE FREE; nothing later holds a
//     pointer to it.
//   * it ALIASES the slot word — an OwningDictionary's class pointer and a
//     borrowing `Dictionary<K, string>`'s co-owned `bz_string*` are both
//     `valDecoded = slotV` — so the snapshot's entries point INTO objects the
//     table owns. Discharging those at the source free hands the caller an
//     enumerator over freed memory. They must move to the WRAPPER'S slot-1
//     Dispose, at the caller's scope end, after iteration. This is the same
//     conclusion #3354 (`LinkedList`) and #3450 (`Stack`) reached for their
//     families.
//
// `Emitter::dictSnapshotValueDisposers` makes that split, and it makes it by
// re-running the SAME cascade the scope-end dictionary disposal already runs
// (`elementPolicyFor(V, owning)` -> `dictValueBoxFreer` -> the #429 borrowing
// string release), so the UNION of its two outputs is exactly the one disposer a
// scope-end free would have selected. No new classifier: the only thing added is
// the routing of each arm to the moment that is safe for it.
//
// The deferred half is installed on `wrapDictionaryEntryArrayAsIEnumerable`,
// which already existed as the #2522 string-KEY Dispose walker; it now takes
// `releaseKey` and `valDisposer` and walks field 0 and/or field 1 of each
// entry. Both are part of the cache key of the Dispose / vtable globals it
// builds, per #3450's discipline and #3570's measured collapse.
//
// ORACLE. Every row is `runAotChecked` — the generated program is compiled with
// AddressSanitizer + LeakSanitizer, so `r.ran` is simultaneously a no-fault,
// no-double-free and NO-LEAK assertion. That is load-bearing here: a plain
// `runAot` cannot see a leak at all, so the same rows on `runAot` would be
// vacuous. Every leak row is loop-amplified 200x, because a one-shot LSan row
// passes on BOTH sides of the fix while the allocation is still reachable at
// exit.
//
// COVERAGE.
//   (A) `Dictionary<int, string>` with INTERPOLATED values, 200x — the pin.
//       Pre-fix: 14580 B in 800 allocations. Post-fix: clean, total=1000.
//   (B) The same shape with STRING LITERAL values — clean on both sides; the
//       row that fails if the deferred release over-releases a sentinel.
//   (C) `Dictionary<string, string>` — BOTH obligations discharged by one
//       Dispose (the rebuilt key at field 0, the inherited value +1 at
//       field 1), which is the combination the widened cache key exists for.
//   (D) `Dictionary<int, int?>` — the "second door" recorded on issue 3568:
//       boxed nullable payloads, freed at the source free because the decode
//       copied them out. Newly ADMITTED by this commit.
//   (E) `Dictionary<int, Color>` — an enum value. Newly ADMITTED.
//   (F) `Dictionary<Color, int>` — an enum key. Newly ADMITTED.
//   (G) A BORROWED `Dictionary<int, int?>` at the ARGUMENT face — the source
//       outlives the coercion and keeps owning its boxes, so neither disposer
//       may fire. The row that fails if the split ignores `srcOwnedByCallee`.
//   (H) A callee-owned `Set<int>` at an `IEnumerable<int>` head — the
//       keys-only caller, which must keep passing an explicit null.
//   (I) `Dictionary<int, int>` — the v0.283 shape, unchanged.
//
// ★ WHAT THIS COMMIT STILL DECLINES, and why it is not this issue any more. A
// RECORD-, CLASS- or INTERFACE-typed value stays outside the shape gate. Its
// DISPOSAL is solved — ARM 1 and ARM 2 of `dictSnapshotValueDisposers` cover the
// owned class pointer and the record box, and an `OwningDictionary<int, Item>`
// iterated 200x measured LSan clean on a build with the gate widened. What is
// not solved is issue 3563: on the WRAPPED entry face `x.Value.<member>` fails
// to lower (the #2961 internal, "store dropped"), so a VALID program reading a
// field of such a value computes 0 — measured `n=400 total=0` where the in-place
// control computes the right answer. Admitting those shapes would trade a
// deterministic SIGSEGV for a quiet wrong answer, which is the trade #3519 was
// narrowed to avoid. They come in with 3563. No row here asserts that SIGSEGV:
// a row pinning a crash signature would pin a defect as expected behaviour, and
// it would be oracle-dependent besides (ASan catches the fault and `_exit`s).

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 9 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 9 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is now LOUD — the row FAILS — instead of being absorbed by a guard
// that reports a clean pass. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// (Never put a C++ `//` comment inside these literals: it is lexed as Bezel
// source and comments out the rest of the program. Bezel's own comment forms
// are `//`, `/* ... */` and `///` -- spec §1.5, which is exhaustive.)

const char* const kEnumColor =
    "public enum Color  Red := 1, Green := 2, Blue := 3;  end Color; ";

}  // namespace

// ---------------------------------------------------------------------------
// (A) THE PIN. A callee-owned `Dictionary<int, string>` whose values are BUILT
//     at run time, iterated 200x under LSan.
//   per §28.9 (the RETURN coercion frees the source), stdlib §4.8 (entry
//   iteration), §28.2 (dispose exactly once)
//
// `$"a{k}"` is an interpolation, so each value is a fresh ref-counted
// `bz_string` the table takes a +1 on (#429). Pre-fix the source free passed a
// null disposer and every one of those +1s leaked: 14580 B in 800 allocations.
// Post-fix the snapshot INHERITS the table's +1 (the source free still passes
// null for this shape, deliberately) and the wrapper's slot-1 Dispose releases
// it once, after iteration — so the `x.Value.Length` read below is also the
// use-after-free assertion for the tempting wrong fix.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_snapshot_value_disposer_3568,
           callee_owned_string_valued_dictionary_releases_its_values_28_9) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Mk(k: int): IEnumerable<DictionaryEntry<int, string>> "
        "    var d := new Dictionary<int, string>(); "
        "    d[1] := $\"a{k}\"; "
        "    d[2] := $\"bb{k}\"; "
        "    return d; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e := Mk(7); "
        "      for var x in e do total := total + x.Value.Length; end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix on 5d052b3: rc 1, "14580 byte(s) leaked in 800 allocation(s)".
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x (len("a7") + len("bb7")) == 200 x (2 + 3) == 1000.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}

// ---------------------------------------------------------------------------
// (B) The SENTINEL control — the same shape with string LITERAL values.
//   per §28.9 (release is null- and sentinel-safe)
//
// A literal's `bz_string` is not a heap producer, so its release is a no-op and
// this row was clean before the fix. It is here to fail if the deferred release
// is applied where no +1 was ever taken: an over-release on a literal would show
// up as an ASan fault or a double free, not as a leak.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_snapshot_value_disposer_3568,
           string_literal_values_are_not_over_released_8_6) {
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
    // Identical on both sides of the fix.
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x (2 + 3) == 1000.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=1000"));
}

// ---------------------------------------------------------------------------
// (C) BOTH obligations at once: `Dictionary<string, string>` with LITERAL keys
//     and BUILT values, 200x under LSan.
//   per stdlib §4.8, §28.11, §28.9
//
// One `Dispose` now walks each entry twice over: `bz_string_release` on the
// REBUILT key at field 0 (the #2522 job) and the inherited value release on
// field 1. This is the combination the widened cache key exists for — before
// #3568 the entry-array wrapper was minted only for the key job, so a
// string-valued dictionary with string keys had no place to put the value
// release at all.
//
// The keys are LITERALS on purpose: a `Dictionary<string, V>` does not release
// its own string KEYS at scope end (the `dictStringKeys` pre-free pass is set
// only for `Set`), so a built key leaks 14580 B in 800 allocations with or
// without any coercion — a separate, pre-existing defect that would mask this
// row's own signal.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_snapshot_value_disposer_3568,
           string_keyed_and_string_valued_dictionary_discharges_both_28_11) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Mk(k: int): "
        "      IEnumerable<DictionaryEntry<string, string>> "
        "    var d := new Dictionary<string, string>(); "
        "    d[\"k1\"] := $\"a{k}\"; "
        "    d[\"k22\"] := $\"bb{k}\"; "
        "    return d; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e := Mk(7); "
        "      for var x in e do "
        "        total := total + x.Key.Length + x.Value.Length; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x ((2 + 2) + (3 + 3)) == 2000.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=2000"));
}

// ---------------------------------------------------------------------------
// (D) NEWLY ADMITTED: a NULLABLE value type. `Dictionary<int, int?>`, 200x.
//   per §7 (nullable value types), stdlib §4.8, §28.9
//
// This is the "second door" recorded on issue 3568: `int?` is
// `Kind::Primitive` with `nullable` true (there is no `Kind::Nullable`), so
// a kind-only shape gate ADMITTED it while the null disposer leaked its boxed
// payloads — measured 3200 B in 400 allocations at 200x. #3519's gate declined
// it explicitly as containment; this commit ADMITS it, because the decode
// copies the tagged struct out of the box (`denum.vnv`) and the source free now
// reclaims the box.
//
// The observable is deliberately the two spellings that are VALID on every
// face: a null comparison and the key sum. (`x.Value.Value` is a compile ERROR
// on a plain `int?` local and on the in-place iteration path — "no member
// `Value` on primitive type `int`" — so a program spelling it is ill-typed
// regardless of this arm.)
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_snapshot_value_disposer_3568,
           nullable_valued_dictionary_frees_its_boxes_7) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Mk(): IEnumerable<DictionaryEntry<int, int?>> "
        "    var d := new Dictionary<int, int?>(); "
        "    d[1] := 10; "
        "    d[2] := null; "
        "    return d; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var present := 0; "
        "    var keys := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e := Mk(); "
        "      for var x in e do "
        "        keys := keys + x.Key; "
        "        if x.Value <> null then present := present + 1; end if; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"keys={keys} present={present}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix on 5d052b3: the shape gate declined `int?`, so the coercion fell
    // through to the raw `bz_dict_t*` and the program SIGSEGV'd on the first
    // iteration (rc 139).
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x (1 + 2) == 600 keys; one non-null value per iteration.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "keys=600 present=200"));
}

// ---------------------------------------------------------------------------
// (E) NEWLY ADMITTED: an ENUM value. `Dictionary<int, Color>`, 200x.
//   per §10 (enums are integer-backed), stdlib §4.8
//
// An enum lowers to its underlying integer, so the slot holds no heap word at
// all and BOTH disposers stay null — the widening here is purely about the
// shape gate, and the row proves the decode's scalar arm handles a
// `Kind::Named` enum the same way it handles `int`.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_snapshot_value_disposer_3568,
           enum_valued_dictionary_is_inside_the_shape_gate_10) {
    const std::string src =
        std::string("module M; ") + kEnumColor +
        "public class Main "
        "  public static method Mk(): IEnumerable<DictionaryEntry<int, Color>> "
        "    var d := new Dictionary<int, Color>(); "
        "    d[1] := Color.Red; "
        "    d[2] := Color.Blue; "
        "    return d; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var keys := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e := Mk(); "
        "      for var x in e do "
        "        keys := keys + x.Key; "
        "        if x.Value = Color.Blue then total := total + 3; end if; "
        "        if x.Value = Color.Red then total := total + 1; end if; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"keys={keys} total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix on 5d052b3: `Color` is `Kind::Named`, declined, SIGSEGV rc 139.
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 x (1 + 2) == 600 keys; 200 x (1 + 3) == 800.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "keys=600 total=800"));
}

// ---------------------------------------------------------------------------
// (F) NEWLY ADMITTED: an ENUM key. `Dictionary<Color, int>`, 200x.
//   per §10, stdlib §4.8
//
// An enum key takes `bz_dict_keys_gen`'s generic-key table: the decode loads
// the key BY VALUE out of the malloc'd raw-key copy and frees that copy, so no
// ownership crosses into the snapshot and no key release is needed. This is the
// half of the widening that has nothing to do with the value disposer, and it
// is the row that fails if the K axis is widened without the `free(keyPtr)`
// that the generic-key arm already emits.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_snapshot_value_disposer_3568,
           enum_keyed_dictionary_is_inside_the_shape_gate_10) {
    const std::string src =
        std::string("module M; ") + kEnumColor +
        "public class Main "
        "  public static method Mk(): IEnumerable<DictionaryEntry<Color, int>> "
        "    var d := new Dictionary<Color, int>(); "
        "    d[Color.Red] := 10; "
        "    d[Color.Blue] := 20; "
        "    return d; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var reds := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e := Mk(); "
        "      for var x in e do "
        "        total := total + x.Value; "
        "        if x.Key = Color.Red then reds := reds + 1; end if; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"reds={reds} total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Pre-fix on 5d052b3: SIGSEGV rc 139.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "reds=200 total=6000"));
}

// ---------------------------------------------------------------------------
// (G) The BORROWED face. The same nullable-valued dictionary passed as an
//     ARGUMENT, 200x under LSan.
//   per §28.3 ("storage lends"), §28.9
//
// `wrapAsIEnumerableForTarget` serves the argument position too, and there the
// source is a named local that keeps owning its boxes and disposes them at its
// OWN scope end. Both disposers must therefore stay null: the split is computed
// only under `srcOwnedByCallee`. A fix that derived them unconditionally would
// DOUBLE-FREE every box here, which ASan reports as a heap error rather than a
// leak — so this row and row (D) fail in opposite directions.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_snapshot_value_disposer_3568,
           borrowed_nullable_valued_dictionary_argument_is_not_double_freed_28_3) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Count(e: IEnumerable<DictionaryEntry<int, int?>>): int "
        "    var n := 0; "
        "    for var x in e do "
        "      if x.Value <> null then n := n + 1; end if; "
        "    end for; "
        "    return n; "
        "  end Count; "
        "  public static method Main(): int "
        "    var present := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var d := new Dictionary<int, int?>(); "
        "      d[1] := 10; "
        "      d[2] := null; "
        "      present := present + Count(d); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"present={present}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "present=200"));
}

// ---------------------------------------------------------------------------
// (H) The KEYS-ONLY caller. A callee-owned `Set<int>` at an `IEnumerable<int>`
//     head, 200x under LSan.
//   per stdlib §4.10, §28.9
//
// The dict-backed `Set` arm shares `freeDictSourceNow` and must keep passing an
// explicit null: a set's hash table has keys and no values, so a value disposer
// there would be called on whatever the value word happens to be. This row is
// the guard that the two-argument overload did not quietly hand the Set arm a
// dictionary's disposer.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_snapshot_value_disposer_3568,
           callee_owned_set_return_still_frees_keys_only_4_10) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Mk(): IEnumerable<int> "
        "    var s := new Set<int>(); "
        "    _ := s.TryAdd(1); "
        "    _ := s.TryAdd(2); "
        "    return s; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e := Mk(); "
        "      for var x in e do total := total + x; end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Identical on both sides of the fix.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=600"));
}

// ---------------------------------------------------------------------------
// (I) The v0.283 shape, unchanged. `Dictionary<int, int>`, 200x.
//   per stdlib §4.8
//
// Scalars carry nothing, so both disposers are null and the wrap keeps the
// plain owning-array wrapper. The row that fails if the routing condition
// (`isStringKey || snapshotValDisposer`) accidentally sends every dictionary
// through the entry-array wrapper.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_snapshot_value_disposer_3568,
           scalar_dictionary_return_is_unchanged_4_8) {
    const std::string src =
        "module M; "
        "public class Main "
        "  public static method Mk(): IEnumerable<DictionaryEntry<int, int>> "
        "    var d := new Dictionary<int, int>(); "
        "    d[1] := 10; "
        "    d[2] := 20; "
        "    return d; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e := Mk(); "
        "      for var x in e do total := total + x.Value; end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // Identical on both sides of the fix.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=6000"));
}
