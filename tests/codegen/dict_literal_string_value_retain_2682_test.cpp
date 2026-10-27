// Codegen / disposal — a `Dictionary<K, string>` COLLECTION LITERAL stored its
// string values without taking the dictionary's +1, while every release site
// released one. Two flat dictionaries sharing one runtime-built string then
// double-freed it. Issue #2682.
//
// Spec: §15.4 "Collection literals" (a collection literal CONSTRUCTS the
// collection — it is the same construction the equivalent `new` + `.Add` writes,
// so it must take the same ownership), §28.9 "Runtime-managed types" (`string`
// is refcounted: every co-owner takes a reference and releases exactly one),
// §28.11 "Collection element ownership" (a borrowing `Dictionary<K, string>`
// co-owns a +1 per stored value). Spec revision v0.237.
//
// THE BUG. `materializeHeapDictLiteral` (LlvmBackend_Setup.cpp) lowered the
// literal `["k": s]` to `bz_dict_set_str(dict, k, v)` with NO `emitStringRetain`
// on `v` — the only insert path for a borrowing `Dictionary<K,string>` that did
// not. `.Add(k, v)` retains (LlvmBackend_Calls.cpp, the `dictIsStringValue`
// guard) and `d[k] := s` retains (LlvmBackend_Assign.cpp, `isStrVal`), and the
// RELEASE side fires unconditionally: `registerUsingDisposal`'s dictionary
// branch and `buildCollectionTempDisposal` both install
// `stringElementReleaseThunk()`, and `.Clear` / `.Remove` / indexer-overwrite
// pass it to the disposing runtime entries. So a literal-built dictionary
// released a +1 it never took. ONE such dictionary survived by accident — it
// consumed the borrowed string's only other reference — which is why this went
// unnoticed; TWO dictionaries sharing one string consumed it twice:
//   AddressSanitizer: SEGV in __asan::Allocator::Deallocate
//     #2 bz_string_release  #3 bz$strElemRelease  #4 bz_dict_free
// and, WITHOUT ASan, `free(): invalid pointer` (SIGABRT, exit 134). Both builds
// are separate oracles and both were red.
//
// THE FIX. Mirror `.Add`'s guard verbatim in `materializeHeapDictLiteral`:
// retain a non-nullable `string` value of a BORROWING dictionary
// (`Dictionary`/`SortedDictionary`; `OwningDictionary` is excluded because its
// value parameter is `owned` — a transfer — and no release site installs the
// thunk for it), unless the value expression is already a fresh +1 producer
// (`isPlusOneStringProducer`), in which case the literal TRANSFERS that
// reference rather than double-retaining it. The Array/Stack/Queue element arm
// of `materializeHeapCollectionLiteral` and `materializeHeapSetLiteral` already
// did exactly this; the dictionary-value arm was the omission.
//
// ⚠ WHY THE STRING MUST BE BUILT AT RUNTIME. `"ab" + "cd"` constant-folds to an
// RC-SENTINEL string, and `bz_string_release` is a NO-OP on a sentinel — a probe
// written that way passes whether or not the bug is present. Every program below
// builds its string with interpolation at run time (`$"v-{i}"`).
//
// ⚠ WHY THE SHARED STRING IS A PARAMETER, NOT A LOCAL. A `var s: string := $"…"`
// that escapes into a collection literal is dropped from `stringReleaseEligible_`
// (#422: membership is the only thing that arms a scope-end release, and the
// escape walk is conservative), so such a local leaks its +1 — pre-existing, and
// `.Add` leaks identically (measured: both the literal and the `.Add` form leak
// the same 731 B / 40 allocations). LSan would then fail these tests for a
// reason that has nothing to do with #2682. Passing the string as a borrowed
// PARAMETER keeps the caller's +1 accounted for, so the programs below are
// exactly leak-neutral and `runAotChecked` observes ONLY the #2682 balance.
//
// OBSERVABILITY. `runAotChecked` links and runs the generated code under
// ASan/LSan, so BOTH failure directions flip `r.ran` to false: a double-free /
// SEGV (too many releases — the bug) and a leak (too many retains — an
// over-correction). The exit code independently pins the values read back out of
// both dictionaries, so a premature free would corrupt it as well. Each test
// FAILS without the fix (ASan SEGV in bz_string_release) and PASSES with it.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// (A) The #2682 repro, reduced to a leak-neutral shape: TWO flat
// `Dictionary<string,string>` literals in one scope share the same runtime-built
// string, borrowed as a parameter. Pre-fix each dictionary released a +1 neither
// took → ASan SEGV inside bz_dict_free. 20 iterations so a stale free is hit
// repeatedly rather than tolerated once.
// Value: 2 * (sum of "v-1".."v-20" lengths = 71) = 142.
const char* kTwoFlatDictsShareStringKey =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var d1: Dictionary<string, string> := [\"k\": s]; "
    "    var d2: Dictionary<string, string> := [\"k\": s]; "
    "    return d1[\"k\"].Length + d2[\"k\"].Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "   // runtime-built, NOT folded
    "    end; "
    "    return total; "                            // 142
    "  end Main; "
    "end C;";

// (B) The GENERIC-KEY table (`bz_dict_set_gen`, not `bz_dict_set_str`) — the
// other half of materializeHeapDictLiteral's insert switch. The value side is
// what carries the obligation (§28.11: a dictionary owns its values), so an
// int-keyed dictionary is affected identically. Value: 142.
const char* kTwoGenericKeyDictsShareString =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var d1: Dictionary<int, string> := [1: s]; "
    "    var d2: Dictionary<int, string> := [1: s]; "
    "    return d1[1].Length + d2[1].Length; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 142
    "  end Main; "
    "end C;";

// (C) The TRANSFER arm: the literal's value is itself a fresh +1 producer
// (`$"v-{i}"` inline), so the fix must NOT retain — the literal takes over the
// producer's reference. A fix that retained unconditionally would leak one
// string per iteration and LSan would fail this. Value: 71.
const char* kProducerValueTransfersNotRetained =
    "module M; class C "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var d: Dictionary<string, string> := [\"k\": $\"v-{i}\"]; "
    "      total := total + d[\"k\"].Length; "
    "    end; "
    "    return total; "                            // 71
    "  end Main; "
    "end C;";

// (D) The #2660 interaction: an INTERIOR `Dictionary<string,string>` node of a
// nested literal shares its string with a flat dictionary. #2660 had to free
// such a node with a NULL child disposer precisely because construction took no
// retain (a bounded string leak, marked revertable in `collectionNodeFreeThunk`).
// With the retain in place that narrowing is reverted, so this program is now
// balanced in BOTH directions: LSan catches the leak the narrowing used to cost,
// ASan catches the double-free reverting it would have caused without #2682.
// Value: 20 * ("v-i".Length + outer.Count=1) = 71 + 20 = 91.
const char* kInteriorNodeAndFlatDictShareString =
    "module M; class C "
    "  public static method Two(s: string): int "
    "    var outer: Dictionary<string, Dictionary<string, string>> := "
    "        [\"o\": [\"k\": s]]; "
    "    var flat: Dictionary<string, string> := [\"k\": s]; "
    "    return flat[\"k\"].Length + outer.Count; "
    "  end Two; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + C.Two($\"v-{i}\"); "
    "    end; "
    "    return total; "                            // 91
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #2682 repro — two flat string-keyed dictionary literals sharing one
// runtime-built string are ASan/LSan-clean. §15.4 / §28.9 / §28.11.
// =====================================================================
BEZEL_TEST(codegen_dict_literal_string_value_retain_2682,
           two_flat_dict_literals_share_string_no_double_free_spec_28_11) {
    JitResult r = runAotChecked(kTwoFlatDictsShareStringKey);
    BEZEL_EXPECT_TRUE(r.ran);          // no double-free (bug) and no leak
    BEZEL_EXPECT_EQ(r.exitCode, 142);  // both dictionaries still read back
}

// =====================================================================
// (B) The generic-key insert arm (bz_dict_set_gen) is affected identically —
// the VALUE carries the obligation, not the key. §28.11.
// =====================================================================
BEZEL_TEST(codegen_dict_literal_string_value_retain_2682,
           two_generic_key_dict_literals_share_string_no_double_free_spec_28_11) {
    JitResult r = runAotChecked(kTwoGenericKeyDictsShareString);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 142);
}

// =====================================================================
// (C) A +1 producer value is TRANSFERRED, not retained — pins the
// isPlusOneStringProducer half of the fix (an unconditional retain leaks here).
// §15.4 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_dict_literal_string_value_retain_2682,
           producer_value_transfers_without_extra_retain_spec_28_9) {
    JitResult r = runAotChecked(kProducerValueTransfersNotRetained);
    BEZEL_EXPECT_TRUE(r.ran);         // no leak from a double-retain
    BEZEL_EXPECT_EQ(r.exitCode, 71);
}

// =====================================================================
// (D) An interior nested-literal node sharing a string with a flat dictionary —
// the #2660 narrowing is reverted and balanced. §15.4 / §28.11.
// =====================================================================
BEZEL_TEST(codegen_dict_literal_string_value_retain_2682,
           interior_node_and_flat_dict_share_string_balanced_spec_15_4) {
    JitResult r = runAotChecked(kInteriorNodeAndFlatDictShareString);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 91);
}
