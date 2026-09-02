// Codegen / silent misread — fix #3570 (§29.2 / §28.9 / §4.8, v0.286):
// `ensureArrayEnumMachinery` and `wrapOwnedArrayAsIEnumerable` keyed their
// cached struct type, `MoveNext`, `Current` and vtable on an **element-name
// string that collapsed every non-integer, non-pointer element type to the
// single letter `"f"`**:
//
//     std::string elemName = elemTy->isIntegerTy()
//         ? ("i" + std::to_string(elemTy->getIntegerBitWidth()))
//         : (elemTy->isPointerTy() ? "ptr" : "f");
//
// Every **struct** element type therefore shared one `bz$arrenum$f$…` family.
// `Current` additionally bakes the element stride `esz` in **as a constant**
// (computed per creation, then frozen into the emitted function). So two
// different struct element types in one module get **one** `bz$arrenum$f$Current`,
// carrying the **first creator's** element type and stride. `DictionaryEntry<int,int>`
// is 8 bytes and `DictionaryEntry<string,int>` is 16; whichever is minted
// first wins, and the other reads its entries at the wrong stride.
//
// There is **no IR-verifier backstop**: the call is made through the vtable
// slot with a locally constructed `FunctionType`, so LLVM sees a well-typed
// call site — silent misread rather than a verification failure.
//
// THE REPRO. Two dictionary coercions to `IEnumerable<DictionaryEntry<K,V>>`
// heads in one module, ordered so the smaller entry type mints first, both
// iterated and summed. The pre-fix value is `t=30` (the int-keyed entries'
// 10 + 20, with the string-keyed entries silently misread as zero / wrong
// bytes); the post-fix value is `t=33` (10 + 20 + 1 + 2). The method names
// are load-bearing — they order the two mints, exactly as #3450's regression
// file's rows (H)/(I) order theirs.
//
// THE FIX. Include the element type's `allocSize` in the cache key for the
// non-integer / non-pointer branch (the issue's suggested `f8` / `f16` form),
// applied at all three sites that compute the same expression: `ensureArrayEnumMachinery`,
// `wrapOwnedArrayAsIEnumerable` and `wrapDictionaryEntryArrayAsIEnumerable`.
// Integer and pointer kinds keep their discriminating `i<bitwidth>` and
// `"ptr"` segments; only the `"f"` fallback broadens.
//
// THE THIRD SITE. `wrapDictionaryEntryArrayAsIEnumerable` (the string-keyed
// dictionary materialisation) also derives `plainPrefix` from the same scheme
// so that it shares the plain MoveNext/Current with `wrapOwnedArrayAsIEnumerable`'s
// non-string-keyed sibling. A fix that touched the array sites only and missed
// this one would re-collide the two string-keyed siblings across `wrapDictionaryAsIEnumerable`.
// All three change together.
//
// ORACLE. `runAot` — the pre-fix symptom is silent (no fault, exit 0,
// wrong answer) on both oracles, so #2633's "neither dominates" rule does not
// apply: the plain build's stdout is the load-bearing indicator. A regression
// row that pinned a SEGV would be vacuous on both builds; pinning the value
// `t=33` is what the row must assert.
//
// COVERAGE.
//   (A) The repro: two `Dictionary<K,V>` coerced in one module, summed.
//       Pre-fix `t=30`; post-fix `t=33`.
//   (B) The reverse order — wider entry mints first, narrower second.
//       Pre-fix `t=30` (the wider's stride wins, the narrower is misread);
//       post-fix `t=33`. The order-matters-ness is what distinguishes this
//       from a benign `t=30` collision.
//   (C) The standalone `Dictionary<string,int>` coercion, control — a single
//       entry type in the module, no cache-collision surface. Post-fix identical
//       to pre-fix; this is the row that says the allocSize suffix did not
//       regress the simple shape.
//   (D) Verified negative — `Dictionary<int,int>` alone. Same shape as (C),
//       integer key. The cache key for this is `f8`, identical to pre-fix for
//       the integer-element path which was unaffected.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A `//` comment inside a C++ string literal holding Bezel source lexes as
// Bezel source and comments out the rest of the program; the test passes for
// the wrong reason. Bezel's own comment forms are `//`, `/* ... */` and `///`
// (spec §1.5, exhaustive) — none of those are used here.
bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Two coercion sites in one module: AaaSmall mints first (the 8-byte entry
// type wins the shared `bz$arrenum$f$Current`), ZzzWide second (the 16-byte
// entry type inherits the 8-byte stride).
const char* const kTwoDictRepro =
    "module M; "
    "public class Main "
    "  public static method AaaSmall(): IEnumerable<DictionaryEntry<int, int>> "
    "    var d := new Dictionary<int, int>(); "
    "    d[1] := 10; "
    "    d[2] := 20; "
    "    return d; "
    "  end AaaSmall; "
    "  public static method ZzzWide(): IEnumerable<DictionaryEntry<string, int>> "
    "    var d := new Dictionary<string, int>(); "
    "    d[\"aa\"] := 1; "
    "    d[\"bb\"] := 2; "
    "    return d; "
    "  end ZzzWide; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    using var a := AaaSmall(); "
    "    for var x in a do t := t + x.Value; end for; "
    "    using var b := ZzzWide(); "
    "    for var y in b do t := t + y.Value; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// Same two coercions, REVERSE order so the wider entry type mints first.
// Pre-fix `t=30` here too: the wider stride (16) wins, the narrower (8) is
// misread at 16-byte steps — its iteration over the int-keyed entries walks
// past the array's tail and yields zeros / garbage from beyond the data
// buffer.
const char* const kTwoDictReproReversed =
    "module M; "
    "public class Main "
    "  public static method ZzzWide(): IEnumerable<DictionaryEntry<string, int>> "
    "    var d := new Dictionary<string, int>(); "
    "    d[\"aa\"] := 1; "
    "    d[\"bb\"] := 2; "
    "    return d; "
    "  end ZzzWide; "
    "  public static method AaaSmall(): IEnumerable<DictionaryEntry<int, int>> "
    "    var d := new Dictionary<int, int>(); "
    "    d[1] := 10; "
    "    d[2] := 20; "
    "    return d; "
    "  end AaaSmall; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    using var b := ZzzWide(); "
    "    for var y in b do t := t + y.Value; end for; "
    "    using var a := AaaSmall(); "
    "    for var x in a do t := t + x.Value; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// Single-coercion control: one Dictionary<string,int> coercion, no second
// struct entry type in the module, so the cache has no collision surface.
// Post-fix identical to pre-fix — value `t=3`.
const char* const kWideOnly =
    "module M; "
    "public class Main "
    "  public static method Mk(): IEnumerable<DictionaryEntry<string, int>> "
    "    var d := new Dictionary<string, int>(); "
    "    d[\"a\"] := 1; "
    "    d[\"b\"] := 2; "
    "    return d; "
    "  end Mk; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    using var e := Mk(); "
    "    for var y in e do t := t + y.Value; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

// Same shape, narrower entry type. Cache key `f8` is the post-fix identity for
// this type; pre-fix identical (also the only struct in the module).
const char* const kSmallOnly =
    "module M; "
    "public class Main "
    "  public static method Mk(): IEnumerable<DictionaryEntry<int, int>> "
    "    var d := new Dictionary<int, int>(); "
    "    d[1] := 10; "
    "    d[2] := 20; "
    "    return d; "
    "  end Mk; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    using var e := Mk(); "
    "    for var x in e do t := t + x.Value; end for; "
    "    Console.WriteLine($\"t={t}\"); "
    "    return 0; "
    "  end Main; "
    "end Main;";

}  // namespace

// ---------------------------------------------------------------------------
// (A) The issue's repro, narrow-mints-first order.
//   per §29.2 (RETURN coercion to IEnumerable<DictionaryEntry<K,V>>),
//   §28.9, stdlib §4.8
//
// Pre-fix on c893895 (origin/main): `t=30`. The 8-byte `Current` wins the
// shared `bz$arrenum$f` name, and the 16-byte `DictionaryEntry<string,int>`
// array is read at 8-byte stride — the iteration walks pairs of
// `{key, value}` slots starting at the wrong offset, picking up key bytes
// (the lower 8 of each 16-byte slot) where values are wanted.
// Post-fix: `t=33`.
// ---------------------------------------------------------------------------
BEZEL_TEST(arrenum_struct_element_cache_key_3570,
           two_dictionary_coercions_in_one_module_sum_their_values_29_2) {
    JitResult r = runAot(kTwoDictRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=33"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "t=30"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (B) The reverse order — wider entry type mints first.
//   per §29.2, §28.9, stdlib §4.8
//
// Pre-fix on c893895: still `t=30`. The wider stride (16) wins, and the
// narrower (8-byte `DictionaryEntry<int,int>`) array is read at 16-byte
// stride — the iteration walks past the int entries and reads the freed
// tail. Either order exposes the same `f`-collapse. Post-fix: `t=33`.
// ---------------------------------------------------------------------------
BEZEL_TEST(arrenum_struct_element_cache_key_3570,
           two_dictionary_coercions_in_reverse_order_still_sum_29_2) {
    JitResult r = runAot(kTwoDictReproReversed);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=33"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "t=30"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (C) Single Dictionary<string,int> coercion — control. No second struct
//     entry type in the module, so the cache has no collision surface.
//   per §29.2, stdlib §4.8
//
// Pre-fix and post-fix identical: `t=3`. The row that says the allocSize
// suffix did not regress the simple shape — a fix that produced e.g. an
// "f16" key for a type that should be "f8" would break this row.
// ---------------------------------------------------------------------------
BEZEL_TEST(arrenum_struct_element_cache_key_3570,
           single_wide_dictionary_coercion_is_unchanged_29_2) {
    JitResult r = runAot(kWideOnly);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=3"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (D) Single Dictionary<int,int> coercion — the narrower entry-type control.
//   per §29.2, stdlib §4.8
//
// Cache key post-fix: `f8`. Pre-fix: `f`. Same element type, same shape.
// Pre-fix and post-fix identical: `t=30`.
// ---------------------------------------------------------------------------
BEZEL_TEST(arrenum_struct_element_cache_key_3570,
           single_small_dictionary_coercion_is_unchanged_29_2) {
    JitResult r = runAot(kSmallOnly);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "t=30"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}