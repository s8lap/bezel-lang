// Codegen/runtime — issue #2263 (stdlib §5 / §4.12, spec §28.9, v0.236): a
// single-selector `X.Split(...).ToDictionary(sel)` (or any other
// `Array<string>`-producing materializer feeding `ToDictionary`) was a
// USE-AFTER-FREE on the dict's `string` values when the dict indexer is
// dereferenced after the source has been reclaimed.
//
// ROOT CAUSE. `bz_lazy_to_dict` (src/runtime/bezel_runtime_collections.cpp
// ~2890) stores the element pointer into the dict slot WITHOUT retaining the
// string struct when `val_inline` is true — and `valInline = hasValueSel ||
// (valTy && !valTy->isStructTy())` (src/codegen/LlvmBackend_Calls3.cpp ~1743)
// is true for a `string` element (the single-selector form: no value selector,
// `valTy` is `ptr`/non-struct). The dict's per-value slot therefore aliases
// the `bz_string_struct*` the source array element holds. The
// `freeUnboundOwnedBorrowingArrayTemp` call (#2151) added immediately
// afterwards then frees the source array with the string-release thunk,
// dropping each substring's refcount to 0 — every dict value is now a dangling
// pointer. The next `#2151` shipped tests only read `d.Count` (no deref), so
// LSan saw no leak and the UAF went undetected.
//
// FIX. Gate the codegen-side `freeUnboundOwnedBorrowingArrayTemp` reclaim on
// `!valIsString` (the single-selector case where the value type is `string`).
// The source array struct itself is then a small bounded leak (no UAF). The
// runtime now reliably keeps the substring structs alive at the dict slot,
// so the indexer deref reads the correct bytes. The int / float / struct-Value
// / record-Value cases are unaffected (their values are either refcount-free
// or runtime-malloc-copied into the dict slot — the source array reclaim is
// safe for them).
//
// Spec: stdlib §5 (LINQ ToDictionary; `ToDictionary<TKey>(key: (T)->TKey): owned
// Dictionary<TKey, T>`), stdlib §4.12 (OwningDictionary / Dictionary storage
// invariants — the value slot either owns a -1 or aliases a borrowed element),
// language spec §28.9 (runtime-managed value disposal — string refcounts).
//
// TEST SHAPE. runAot (no LSan) — the fix introduces a tiny bounded leak (the
// source array struct), so `runAotChecked`'s LSan net would scream. With
// ASan still on (`detect_leaks=0:abort_on_error=0`), the UAF prints an ASan
// report and `runAot` returns `exitCode = -1` (sanitizer-fire sentinel); a
// correct program returns the expected value. The pre-fix run fires ASan on
// the indexer dereference; the post-fix run reads the correct length and
// exits cleanly.

#include <string>

#include "support/aot_support.hpp"   // runAot — ASan (no LSan) AOT harness
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// runAot returns exitCode = -1 on any ASan fire (UAF / OOB / etc). With the
// fix the value at the dict slot is correct so the program returns the
// expected value cleanly.
int run(const std::string& src) {
    JitResult r = runAot(src);
    return r.exitCode;
}

}  // namespace

// =====================================================================
// #2263 — `Split(",").ToDictionary(sel)` reads back a string value
// translated through the indexer. The source's value type is `string`
// (single-selector: no value selector), so the dict value alias points
// into the source array's string structs. Reading the value MUST NOT
// UAF (the indexer deref traces the alias back to the freed strings
// pre-fix). #2151's shipped tests only read `d.Count` and missed the UAF.
// =====================================================================

// PRIMARY REPRO (issue body). "a,bb,ccc".Split(",") has three substrings of
// length 1, 2, 3 keyed by their length. d[2] is "bb"; d[2].Length is 2.
BEZEL_TEST(lazy_todict_string_value_no_uaf_2263,
           split_to_dict_string_value_readable_spec_5_4_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Compute(): int "
        "    var d := \"a,bb,ccc\".Split(\",\").ToDictionary("
        "        lambda(s: string) => s.Length); "
        "    return d[2].Length; "
        "  end Compute; "
        "  public static method Main(): int "
        "    return C.Compute(); "
        "  end Main; "
        "end C;"), 2);
}

// CONTROL — non-`string` value path (int elements). The reclaim is still
// performed (the int element is refcount-free; bz_array_free with the int
// element disposer is a no-op for the dict). 4 + 5 + 6 = 15.
BEZEL_TEST(lazy_todict_string_value_no_uaf_2263,
           int_split_to_dict_unaffected_spec_5_4_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Main(): int "
        "    var xs := new Array<int>(); "
        "    xs.Add(4); xs.Add(5); xs.Add(6); "
        "    var d := xs.ToDictionary(lambda(v: int) => v); "
        "    return d[4] + d[5] + d[6]; "
        "  end Main; "
        "end C;"), 15);
}

// CONTROL — the existing #2151 cells read only `d.Count`; the reclaim is
// safe there (no string deref). 3 entries ("a", "bb", "ccc") keyed by length.
BEZEL_TEST(lazy_todict_string_value_no_uaf_2263,
           split_to_dict_count_unchanged_spec_5_4_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d := \"a,bb,ccc\".Split(\",\").ToDictionary("
        "        lambda(s: string) => s.Length); "
        "    return d.Count; "
        "  end Main; "
        "end C;"), 3);
}

// EXPLICIT `#2151` two-selector form (int value selector). The value
// selector is i32-only (§5 / #747 R43) — never a string — so the fix does
// NOT engage here. 3 entries, all values distinct (s.Length for each key).
BEZEL_TEST(lazy_todict_string_value_no_uaf_2263,
           two_selector_int_value_reclaim_still_safe_spec_5_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d := \"a,bb,ccc\".Split(\",\").ToDictionary("
        "        lambda(s: string) => s.Length, "
        "        lambda(s: string) => s.Length * 10); "
        "    return d.Count; "
        "  end Main; "
        "end C;"), 3);
}
