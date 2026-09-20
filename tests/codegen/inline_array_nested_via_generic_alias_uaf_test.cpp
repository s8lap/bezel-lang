// Send-back for fix #4280 (r18 cycle-3 review) — §15.1 (v0.298): "Inline
// arrays may not be elements of inline arrays in the form `[N][M]T` ... The
// current spec admits only one-dimensional inline arrays."
//
// ROOT CAUSE (measured): the direct-syntax rejection for this rule lives
// ONLY in `Resolver::resolveType`'s InlineArray case (src/sema/
// TypeResolution_Names.cpp) — it checks whether a *literally written*
// `[N]T`'s element is itself Kind::InlineArray. `Resolver::
// substituteAliasGenericArgs` (the alias-generic-argument substitution walk
// fix #4188 added for §27.8) has its OWN InlineArray case, which builds
// `nn->arrayElement = ne` from the substituted element with NO re-check at
// all. So a nested `[N][M]T` was constructible through an ordinary generic
// alias with no exotic machinery:
//
//   type Nested<T> := [1]T;
//   ... Wrap(s): Nested<[1]string> { return [inner]; }
//
// compiled clean and produced a live heap-use-after-free — MEASURED,
// `scripts/agent_check.sh --probe --asan`: a real `AddressSanitizer:
// heap-use-after-free`, READ of size 8, freed in `main`, previously
// allocated in `main`, the identical 32-byte `bz.string`-header signature
// the #4254/#4260/#4265/#4280 family measures elsewhere in this codebase.
// The nested-array shape itself (independent of string/UAF) is exactly what
// `[2][3]int` already rejects via the direct-syntax path
// (tests/lit/sema/inline_array_element_count__multi_dimensional_rejected.bzl) —
// this file's `nested_int_element_alias_rejected` row below is the alias-
// substitution sibling of that same lit row.
//
// THE FIX (src/sema/TypeResolution_Names.cpp / TypeResolution_impl.hpp):
// `substituteAliasGenericArgs`'s InlineArray case now checks, after
// substituting the element, whether the RESULT is itself
// `TypeRef::Kind::InlineArray` — and if so, reports the IDENTICAL §15.1
// diagnostic `resolveType`'s direct-syntax check already used, factored
// into one shared `Resolver::reportNestedInlineArrayError` helper (one
// spelling, not a drifting second copy — LANE_RULES §6). `useLoc` (the
// ALIAS REFERENCE SITE's own source location, e.g. the `Nested<[1]string>`
// mention) is threaded through every recursive call so the diagnostic
// reports at the location that actually produces the illegal shape, not the
// alias's own declaration.
//
// BOUNDED, not open-ended: §15.1 admits only one dimension, so
// `arrayElement` can never itself be `Kind::InlineArray` BEFORE
// substitution (that shape is already rejected at the alias's own
// declaration, via the direct-syntax check, when the alias is first
// resolved) — the new check only ever fires exactly where substitution just
// created the illegal shape by replacing a bare type parameter with a
// concrete inline-array argument, and it does not recurse.
//
// NOT generalized beyond InlineArray elements: a generic alias over a
// Record-, Choice-, or Class-DECLARED element continues to work exactly as
// before (this fix touches only the InlineArray case of
// `substituteAliasGenericArgs`) — the four control rows below pin that
// directly.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// PRIMARY PIN — the exact Finding-1 repro (string element, matching the
// live-UAF shape measured above). FAIL-WITHOUT: a real
// AddressSanitizer: heap-use-after-free (measured on 9388b8c, pre-fix).
// PASS-WITH: a clean compile-time reject naming §15.1's own text.
BEZEL_TEST(inline_array_nested_via_generic_alias_uaf,
           nested_string_element_alias_rejected) {
    auto r = runAot(
        "module M; "
        "type NestedAliasUaf<T> := [1]T; "
        "class HelperNestedAliasUaf "
        "  public static method Wrap(s: string): NestedAliasUaf<[1]string> "
        "    var inner: [1]string := [s]; "
        "    return [inner]; "
        "  end Wrap; "
        "end HelperNestedAliasUaf; "
        "class C "
        "  public static method Main(): int "
        "    var outer: NestedAliasUaf<[1]string>; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := HelperNestedAliasUaf.Wrap(s); "
        "    end; "
        "    return outer[0][0].Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an inline array may not be the element of another inline array"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""15.1"));
}

// SIBLING PIN — the nested-array shape itself, independent of string/UAF
// (an int element), mirroring the direct-syntax `[2][3]int` lit row
// (tests/lit/sema/inline_array_element_count__multi_dimensional_rejected.bzl)
// but reached through alias substitution instead of literal syntax. Proves
// the fix is not string-specific — §15.1 applies to any element type.
BEZEL_TEST(inline_array_nested_via_generic_alias_uaf,
           nested_int_element_alias_rejected) {
    auto r = runAot(
        "module M; "
        "type NestedAliasUafInt<T> := [2]T; "
        "class HelperNestedAliasUafInt "
        "  public static method Wrap(x: NestedAliasUafInt<[3]int>): int "
        "    return 0; "
        "  end Wrap; "
        "end HelperNestedAliasUafInt; "
        "class C "
        "  public static method Main(): int "
        "    var a: [3]int := [1, 2, 3]; "
        "    var b: [3]int := [4, 5, 6]; "
        "    var n: NestedAliasUafInt<[3]int> := [a, b]; "
        "    return HelperNestedAliasUafInt.Wrap(n); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an inline array may not be the element of another inline array"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""15.1"));
}

// CONTROL 1/4 — a single-level alias (`Nested<string>` = `[1]string`, no
// nesting at all) must keep compiling and running correctly. Pins that the
// fix's new check only fires on the ILLEGAL (nested) shape, not on every
// InlineArray substitution.
BEZEL_TEST(inline_array_nested_via_generic_alias_uaf,
           single_level_alias_control_stays_clean) {
    auto r = runAot(
        "module M; "
        "type NestedAliasUafOk<T> := [1]T; "
        "class HelperNestedAliasUafOk "
        "  public static method Wrap(s: string): NestedAliasUafOk<string> "
        "    return [s]; "
        "  end Wrap; "
        "end HelperNestedAliasUafOk; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var outer: NestedAliasUafOk<string> := HelperNestedAliasUafOk.Wrap(s); "
        "    return outer[0].Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// CONTROL 2/4 — a generic alias over a RECORD-declared element must keep
// working (the fix touches only the InlineArray case, not Named/Record
// substitution).
BEZEL_TEST(inline_array_nested_via_generic_alias_uaf,
           record_element_alias_control_stays_clean) {
    auto r = runAot(
        "module M; "
        "record StrAliasUafOk(V: string); "
        "type NestedAliasUafRec<T> := [1]T; "
        "class HelperNestedAliasUafRec "
        "  public static method Wrap(v: string): NestedAliasUafRec<StrAliasUafOk> "
        "    return [new StrAliasUafOk(v)]; "
        "  end Wrap; "
        "end HelperNestedAliasUafRec; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var outer: NestedAliasUafRec<StrAliasUafOk> := "
        "HelperNestedAliasUafRec.Wrap(s); "
        "    return outer[0].V.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// CONTROL 3/4 — a generic alias over a CHOICE-declared element must keep
// working.
BEZEL_TEST(inline_array_nested_via_generic_alias_uaf,
           choice_element_alias_control_stays_clean) {
    auto r = runAot(
        "module M; "
        "choice TagAliasUafOk Named(V: string); end TagAliasUafOk; "
        "type NestedAliasUafChoice<T> := [1]T; "
        "class HelperNestedAliasUafChoice "
        "  public static method Wrap(v: string): NestedAliasUafChoice<TagAliasUafOk> "
        "    return [TagAliasUafOk.Named(v)]; "
        "  end Wrap; "
        "end HelperNestedAliasUafChoice; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var outer: NestedAliasUafChoice<TagAliasUafOk> := "
        "HelperNestedAliasUafChoice.Wrap(s); "
        "    return (case outer[0] of Named(n) => n.Length); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// CONTROL 4/4 — a generic alias over a CLASS-declared element must keep
// working.
BEZEL_TEST(inline_array_nested_via_generic_alias_uaf,
           class_element_alias_control_stays_clean) {
    auto r = runAot(
        "module M; "
        "class WidgetAliasUafOk "
        "  public V: string; "
        "  public constructor(v: string) V := v; end constructor; "
        "end WidgetAliasUafOk; "
        "type NestedAliasUafClass<T> := [1 owned]T; "
        "class HelperNestedAliasUafClass "
        "  public static method Wrap(v: string): NestedAliasUafClass<WidgetAliasUafOk> "
        "    return [new WidgetAliasUafOk(v)]; "
        "  end Wrap; "
        "end HelperNestedAliasUafClass; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var outer: NestedAliasUafClass<WidgetAliasUafOk> := "
        "HelperNestedAliasUafClass.Wrap(s); "
        "    return outer[0].V.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 5);
}
