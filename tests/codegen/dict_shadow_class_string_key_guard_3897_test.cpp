// Codegen / refcounting — issue #3897: the built-in-Dictionary string-key
// "safe read" relaxation must not fire for a USER CLASS that merely shares
// one of the three dict-family names (Dictionary / OwningDictionary /
// SortedDictionary).
//
// Spec citations (`docs/Bezel_Language_Spec.md`, v0.294, this worktree's base
// revision): the reserved-type-name paragraph — "The PascalCase standard-
// library names in the table above ... are reserved type names ... This does
// not reserve other standard-library type names ... an ordinary library type
// such as `Array` or `Stream` may still be shadowed by a user type under the
// normal name-resolution rules." `Dictionary` / `OwningDictionary` /
// `SortedDictionary` are NOT in that reserved table (only the PascalCase
// primitive aliases and `String` are), so `class Dictionary<T> { ... }` is
// legal Bezel. Also stdlib §4.8/§4.9/§4.17 (the dict-family methods/indexer
// this relaxation targets) and spec §23.4/§28.9 (indexer index-argument
// ownership, the same rule #2717 pinned for the general case).
//
// THE BUG. `Emitter::isDictTypeName` (LlvmBackend_Calls.cpp) is the canonical
// "is this TypeRef the builtin bz_dict_t*-backed family" test: `kind ==
// Named`, `!declaration` (excludes a user type sharing the name), and
// `common::isDictionaryName(headName())`. It is PRIVATE, so two namespace-
// scope walker structs in LlvmBackend_Setup.cpp that need the identical test
// — StringEscapeWalker::walkCallArgs's `dictMethodStrKeySafe` and
// StringEscapeWalker's IndexExpr arm's `dictStrKeySafe` — each hand-rolled a
// copy that tested ONLY `headName()` against the three-name list, omitting
// `kind == Named && !declaration`. (A third copy, ClosureEscapeWalker's
// GetOrAdd-factory admission, already carried the guard correctly.) For a
// user class named `Dictionary`/`OwningDictionary`/`SortedDictionary`,
// `declaration` is the user's own `TypeSymbol` (non-null) while `headName()`
// still returns the shadowing name — so the two incomplete copies answered
// "yes, builtin, the safe-read relaxation applies" for a receiver that is
// structurally a user class.
//
// THE FIX adds one shared free function, `isBuiltinDictionaryTypeRef`
// (LlvmBackend_impl.hpp), reproducing `isDictTypeName`'s full body, and
// routes all three inline copies plus `Emitter::isDictTypeName` itself
// through it — the #3897 unification.
//
// WHY THIS IS A CORRECTNESS BUG, NOT TIDYING. The relaxation exists to let a
// caller release a string-local KEY argument's own `+1` at scope end, on the
// premise that the built-in dict runtime never retains it (it `strndup`s a
// byte COPY, per fix #3703/#3872/#3886). That premise is void for a
// shadowing USER class: if its own indexer getter or a method simply hands
// the key argument BACK to the caller without minting a fresh `+1` — exactly
// the aliasing-getter shape issue #2717 fixed for the general case
// (`[k: string]: string { get => k; }`, a bare load with no retain) — the
// caller's now-early scope-end release frees the string out from under
// whatever alias the callee handed back. MEASURED (this worktree, pre-fix):
// both rows below are an ASan `heap-use-after-free` in `main`; post-fix, both
// degrade to the SAME safe residual already accepted for other class
// shapes — catalogued as #2746 ITEM 3 (the aliasing-getter exclusion
// #2717 deliberately left out, with this identical 731 B / 40-allocation
// repro), NOT #2717 itself.
// names — a bounded LSan leak, never a memory error.
//
// Case (A) exercises `dictStrKeySafe` (the IndexExpr arm) via a shadowing
// `Dictionary<T>` with `[k: T]: T { get => k; }` — the #2717
// `kNamedLocalIndexPointerElementNullableSink` shape, renamed to the
// shadowing class.
//
// Case (B) exercises `dictMethodStrKeySafe` (the walkCallArgs arm) via a
// GENERIC method `ContainsKey<U>(k: U): U => k;` on the shadowing class. A
// generic method's `sema::MethodSymbol` has non-empty `genericParams`, which
// `walkCallArgs`'s `ms`-gated #1430 general relaxation excluded WHOLESALE
// (`if (ms && (... || !ms->genericParams.empty() || ...)) ms = nullptr;`)
// — so, unlike an ORDINARY (non-generic) resolved method (already covered,
// soundly, by #1430 regardless of this fix), this shape isolated
// `dictMethodStrKeySafe`'s OWN structural-name-only relaxation as the sole
// path that could mark the argument safe, exactly mirroring case (A)'s
// isolation of `dictStrKeySafe`.
//
// #5300/#5301 (§28.3 *Bare-type-parameter returns*) narrowed that exclusion
// from WHOLESALE to "generic AND NOT a bare-type-parameter return"
// (`!ms->genericParams.empty() && !methodReturnsBareTypeParam(ctx, ms)`), and
// `U` here is exactly such a return. So the argument is now admitted as a
// borrow source by the general #1430 relaxation, the local's scope-end
// release arms, and case (B) no longer leaks — the sanctioned flip recorded
// at its own row below.
//
// Case (C) is a NEGATIVE control: a shadowing class whose indexer element is
// `int` (cannot alias the `string` key) is unaffected by this fix either way
// — the ordinary user-indexer relaxation (`idxPs`, fix #2717's own
// `indexerElementCannotAliasIndex` gate) already handles it soundly, and
// stays fully leak-free (not merely leak-not-crash) both before and after.
//
// OVER-REJECTION COST, measured (§7 of this run's lane rules): grepped the
// whole test tree for `class Dictionary`/`OwningDictionary`/`SortedDictionary`
// (and record/interface spellings) — every hit is either the stdlib's own
// declared surface (owningdict_getenumerator_2521, sorted_dictionary_class_
// key_3488) or a `tests/conformance/cases/*.bzl` signature-only mirror of the
// REAL stdlib type (no user class shadows the name AND calls a string-keyed
// method/indexer on an instance of it) — zero programs in the existing suite
// exercise the now-excluded path, so this fix costs no in-suite over-
// rejection.
//
// FAIL-WITHOUT / PASS-WITH (this file, filter
// `dict_shadow_class_string_key_guard_3897`, bezel_tests_codegen):
//   PASS-WITH (this commit): (A)'s checked row reports a leak and EXPLICITLY
//   not a memory error; (B)'s checked row reports CLEAN (200 calls, no
//   report at all — see its own comment for the flip and the FORWARD-LOOKING
//   note that sanctioned it); (C)'s checked row reports clean; all three
//   value rows return the documented constant.
//   FAIL-WITHOUT (LlvmBackend_impl.hpp / LlvmBackend_Calls.cpp /
//   LlvmBackend_Setup.cpp reverted to this branch's pre-fix HEAD,
//   bezel_tests_codegen RELINKED via `--area codegen` before rerunning — not
//   just bezelc): (A) and (B)'s checked rows report a genuine ASan
//   `heap-use-after-free` in `main` (`r.ran == false`), which
//   `expectLeakButNoMemoryError` rejects; (C) is unaffected (it never routed
//   through the buggy predicate to begin with).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// ⚠ FORWARD-LOOKING: the two not_armed rows below ASSERT a residual leak
// that is a deliberate, permanently-accepted leak-over-UAF tradeoff —
// #2746 item 3 for the aliasing-getter shape (case A), and the general
// #1430 generic-method escape exclusion for case B. If either underlying
// limitation is ever tightened, these rows FLIP from leak-assertion to
// clean-assertion. That flip is EXPECTED, not a regression: update the
// rows, do not revert the tightening.
//
// CASE B HAS NOW FLIPPED — at #5300/#5301 (the §28.3 bare-type-parameter
// return copy at a runtime-managed substitution tightened walkCallArgs'
// generic-method exclusion, so a plain parameter of a bare-type-parameter
// RETURN is now admitted as a borrow SOURCE and the local's scope-end
// release arms). The case-B row is renamed
// `..._aliasing_return_clean_after_5300_spec_28_9` and now asserts the
// CLEAN outcome — the flip this comment predicted, taken as sanctioned.
// CASE A still pins its #2746-item-3 residual and is deliberately left
// asserting the leak: the aliasing-GETTER exclusion was NOT tightened.
// absent — mirrors string_index_arg_release_2717_test.cpp /
// dict_call_arg_key_local_release_3886_test.cpp.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

std::string sanitizerText(const JitResult& r) {
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    return all;
}

// A checked run that is expected to LEAK (the documented #2717 residual for
// an aliasing accessor) and, in particular, NOT to have freed anything early.
// Mirrors string_index_arg_release_2717_test.cpp's helper of the same name.
void expectLeakButNoMemoryError(const JitResult& r) {
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") != std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("SEGV") == std::string::npos);
}

// =====================================================================
// (A) dictStrKeySafe (IndexExpr arm) — a shadowing `Dictionary<T>` with an
// aliasing indexer. #2717's own `kNamedLocalIndexPointerElementNullableSink`
// shape, renamed. Returns len("y-20") = 4.
// =====================================================================
const char* kShadowDictIndexerAliasingGetter =
    "module M; class Dictionary<T> "
    "  public property [k: T]: T "
    "    get => k; "
    "  end; "
    "end Dictionary; "
    "class C "
    "  public static method Main(): int "
    "    using var e := new Dictionary<string>(); "
    "    var last: string? := null; "
    "    for var i in 1..20 do "
    "      var s := $\"y-{i}\"; "
    "      last := e[s]; "                 // plain store: raw alias, no retain
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      if churn.Length < 0 then return 99; end; "
    "    end; "
    "    return last!.Length; "            // 4
    "  end Main; "
    "end C;";

// =====================================================================
// (B) dictMethodStrKeySafe (walkCallArgs arm) — a shadowing `Dictionary<T>`
// with a GENERIC method named `ContainsKey` (matching the admitted-method
// list purely by name) that returns its own argument unchanged. The generic
// method nulls out `ms` in walkCallArgs (per #1430's own `!ms->genericParams.
// empty()` exclusion), isolating dictMethodStrKeySafe's structural-name-only
// relaxation as the only thing that could mark `s` safe. Returns 4, same as
// (A) — the shapes are deliberately parallel.
//
// AMPLIFIED 10x20 = 200 CALLS (fix-forward at #5300/#5301). When this
// program is asserted CLEAN, LSan has to see 200 rounds of the accounting
// before the "no leak" verdict means anything; at the original 20 it would
// pass on a much smaller sample of the same balance. The OUTER repeat loop
// (rather than widening the inner range) is what keeps the pinned constant
// at 4: the last assignment is still `i = 20`, so `last` is still "y-20".
// The balance under the tightened walker is exactly closed at 200 calls:
// 200 calls mint 200 result +1, 199 are discharged by the plain store into
// `last` overwriting the previous value, and the 200th by `last`'s own
// scope-end release; 200 `s` locals each carry a +1 that their scope-end
// release now discharges (the leak this file used to pin).
// =====================================================================
const char* kShadowDictGenericMethodAliasingReturn =
    "module M; class Dictionary<T> "
    "  public method ContainsKey<U>(k: U): U => k; "
    "end Dictionary; "
    "class C "
    "  public static method Main(): int "
    "    using var e := new Dictionary<string>(); "
    "    var last: string? := null; "
    "    for var round in 1..10 do "
    "      for var i in 1..20 do "
    "        var s := $\"y-{i}\"; "
    "        last := e.ContainsKey(s); "   // plain store: raw alias, no retain
    "        var churn := $\"churn-churn-churn-{round}-{i}-{i}\"; "
    "        if churn.Length < 0 then return 99; end; "
    "      end; "
    "    end; "
    "    return last!.Length; "            // 4
    "  end Main; "
    "end C;";

// =====================================================================
// (C) NEGATIVE CONTROL — a shadowing `Dictionary<T>` whose indexer element is
// `int` (cannot alias the `string` key, fix #2717's own
// `indexerElementCannotAliasIndex` gate). The ordinary user-indexer
// relaxation (`idxPs`) already handles this soundly and independently of
// `dictStrKeySafe`; this fix must not change it. Fully clean (no leak at
// all), both before and after this fix. Returns 20 (1 per iteration).
// =====================================================================
const char* kShadowDictNonAliasingElementStaysClean =
    "module M; class Dictionary<T> "
    "  public property [k: T]: int "
    "    get => 1; "
    "  end; "
    "end Dictionary; "
    "class C "
    "  public static method Main(): int "
    "    using var e := new Dictionary<string>(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"y-{i}\"; "
    "      total := total + e[s]; "
    "    end; "
    "    return total; "                   // 20
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------
// (A)
// ---------------------------------------------------------------------
BEZEL_TEST(dict_shadow_class_string_key_guard_3897,
           shadow_dict_indexer_aliasing_getter_not_armed_spec_23_4) {
    // The load-bearing gate: pre-fix this is `heap-use-after-free` in main,
    // which expectLeakButNoMemoryError rejects explicitly.
    JitResult r = runAotChecked(kShadowDictIndexerAliasingGetter);
    if (wasSkipped(r)) return;
    expectLeakButNoMemoryError(r);
}

BEZEL_TEST(dict_shadow_class_string_key_guard_3897,
           shadow_dict_indexer_aliasing_getter_value_survives_spec_23_4) {
    JitResult r = runAot(kShadowDictIndexerAliasingGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// ---------------------------------------------------------------------
// (B)
// ---------------------------------------------------------------------
BEZEL_TEST(dict_shadow_class_string_key_guard_3897,
           shadow_dict_generic_method_aliasing_return_clean_after_5300_spec_28_9) {
    // BEHAVIOR FLIP, sanctioned by this file's own FORWARD-LOOKING comment
    // above (fix-forward for #5300/#5301, §28.3 *Bare-type-parameter
    // returns* / §28.9, v0.313).
    //
    // WAS: `expectLeakButNoMemoryError(r)` — this row ASSERTED the residual
    // leak, "detected memory leaks" had to be PRESENT. The comment named the
    // pinned limitation precisely: "the general #1430 generic-method escape
    // exclusion for case B", and said a tightening of it flips the row and
    // the row is to be updated rather than the tightening reverted.
    //
    // NOW: the tightening landed. `ContainsKey<U>(k: U): U` declares a bare
    // type-parameter return keyed PRE-substitution, and at `U = string` §28.3's
    // callee-side rule 1 makes the return path a COPY — the call site mints
    // the result's own +1 (LlvmBackend_Calls2.cpp's lowerCallDispatch) while
    // the argument, a PLAIN parameter, is a borrow SOURCE (§28.3 clause 4:
    // "`owned` parameters are never sources", so a plain one is). walkCallArgs
    // no longer nulls `ms` for this callee, so `s`'s scope-end release arms
    // and the per-call leak is gone. The row now asserts the CLEAN outcome,
    // amplified to 200 calls (see the program's comment), and KEEPS the
    // memory-error half of the old helper: arming a release is only safe if
    // it does not free something still read, so heap-use-after-free,
    // double-free and SEGV must all be absent. "no leak" alone would pass on
    // a program that simply stopped running.
    JitResult r = runAotChecked(kShadowDictGenericMethodAliasingReturn);
    if (wasSkipped(r)) return;
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);          // the aliasing return still survives
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("SEGV") == std::string::npos);
}

BEZEL_TEST(dict_shadow_class_string_key_guard_3897,
           shadow_dict_generic_method_aliasing_return_value_survives_spec_28_9) {
    JitResult r = runAot(kShadowDictGenericMethodAliasingReturn);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// ---------------------------------------------------------------------
// (C) negative control
// ---------------------------------------------------------------------
BEZEL_TEST(dict_shadow_class_string_key_guard_3897,
           shadow_dict_non_aliasing_element_stays_leak_free_spec_23_4) {
    JitResult r = runAotChecked(kShadowDictNonAliasingElementStaysClean);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);   // no leak report at all — fully clean
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

BEZEL_TEST(dict_shadow_class_string_key_guard_3897,
           shadow_dict_non_aliasing_element_value_correct_spec_23_4) {
    JitResult r = runAot(kShadowDictNonAliasingElementStaysClean);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}
