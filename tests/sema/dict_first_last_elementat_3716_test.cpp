// Sema — fix #3716 / stdlib §5, §4.17, §4.9 (v0.286).
//
// Pure-sema pins (no codegen/AOT involved — `parseAndCollect` +
// `resolveTypes` + `typecheckExpressions` only) for the two mechanisms
// `src/sema/ExprTypecheck_Calls.cpp` now implements:
//
//   1. `SortedDictionary<K,V>`'s `dictEntryElemTyped` hand-off now types
//      `First`/`Last`/`ElementAt` as `DictionaryEntry<K,V>` (the
//      SortedDictionary-only catch-all no longer clobbers them to the bare
//      value type `V`), while `Single` keeps the OLD behavior (typed as
//      `V`, so a `.Key`/`.Value` access on it rejects) because narrow
//      #3576's codegen arm never implemented `Single` — see #3761. This is
//      the narrowest possible boundary: three names change, one does not,
//      and this file pins exactly that line.
//
//   2. `OwningDictionary<K,V>` gets a NEW, targeted diagnostic for all four
//      names — not a typing rule. Per stdlib §4.9's own prose
//      ("The interface conformance is `IDisposable` only... a borrowing
//      entry view over an owning dictionary is a separate decision
//      deliberately not taken here"), these four names are not part of
//      `OwningDictionary`'s stdlib surface at all, so typing the call (the
//      shape issue #3716 originally suggested, "mirror the Dictionary
//      rule") would be WRONG — it would make `od.First().Key` compile,
//      contradicting the spec. Rejecting it is correct; these rows prove
//      the reject fires at SEMA (before codegen ever sees the call),
//      distinguishing it from the pre-fix silent-degradation shape (#2491)
//      which only ever showed up at codegen/runtime.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

std::string sema(const std::string& body) {
    return "module M; "
           "class R public Tag: int := 0; "
           "  public constructor(t: int) Tag := t; end constructor; "
           "end R; "
           "class C "
           "  public static method T(): int " + body + " end T; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// SortedDictionary — the `dictEntryElemTyped` boundary
// ---------------------------------------------------------------------------

// fix #3716 / stdlib §4.17, §5: `First()` now type-checks — no diagnostic at
// all, `Key`/`Value` resolve on the entry.
BEZEL_TEST(sema_dict_first_last_elementat_3716,
           sorted_dictionary_first_type_checks_clean) {
    auto r = parseSemaAll(sema(
        "    var sd := new SortedDictionary<int, int>(); "
        "    sd.Add(1, 10); "
        "    var fe := sd.First(); "
        "    return fe.Key + fe.Value; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Same for `Last()`.
BEZEL_TEST(sema_dict_first_last_elementat_3716,
           sorted_dictionary_last_type_checks_clean) {
    auto r = parseSemaAll(sema(
        "    var sd := new SortedDictionary<int, int>(); "
        "    sd.Add(1, 10); "
        "    var le := sd.Last(); "
        "    return le.Key + le.Value; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Same for `ElementAt(i)`.
BEZEL_TEST(sema_dict_first_last_elementat_3716,
           sorted_dictionary_element_at_type_checks_clean) {
    auto r = parseSemaAll(sema(
        "    var sd := new SortedDictionary<int, int>(); "
        "    sd.Add(1, 10); "
        "    var ea := sd.ElementAt(0); "
        "    return ea.Key + ea.Value; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `Single()` STAYS a loud reject: no codegen arm exists for it (#3761), so
// this fix deliberately does NOT lift its exclusion. The catch-all types the
// call as `V` (here `int`), so `.Key` rejects as "no member on primitive
// type" — the exact same diagnostic shape the pre-fix containment produced
// for all four names.
BEZEL_TEST(sema_dict_first_last_elementat_3716,
           sorted_dictionary_single_still_rejects_no_codegen_arm_3761) {
    auto r = parseSemaAll(sema(
        "    var sd := new SortedDictionary<int, int>(); "
        "    sd.Add(1, 10); "
        "    var se := sd.Single(); "
        "    return se.Key + se.Value; "));
    BEZEL_EXPECT_TRUE(hasError(r, "no member `Key` on primitive type"));
}

// ---------------------------------------------------------------------------
// OwningDictionary — targeted "does not expose" reject, all four names,
// firing at SEMA (no codegen involved in this test file at all)
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_dict_first_last_elementat_3716,
           owning_dictionary_first_rejected_at_sema_4_9) {
    auto r = parseSemaAll(sema(
        "    using var od := new OwningDictionary<int, R>(); "
        "    od.Add(1, new R(1)); "
        "    var fe := od.First(); "
        "    return fe.Key; "));
    BEZEL_EXPECT_TRUE(
        hasError(r, "`OwningDictionary<K, V>` does not expose `First`"));
}

BEZEL_TEST(sema_dict_first_last_elementat_3716,
           owning_dictionary_last_rejected_at_sema_4_9) {
    auto r = parseSemaAll(sema(
        "    using var od := new OwningDictionary<int, R>(); "
        "    od.Add(1, new R(1)); "
        "    var le := od.Last(); "
        "    return le.Key; "));
    BEZEL_EXPECT_TRUE(
        hasError(r, "`OwningDictionary<K, V>` does not expose `Last`"));
}

BEZEL_TEST(sema_dict_first_last_elementat_3716,
           owning_dictionary_single_rejected_at_sema_4_9) {
    auto r = parseSemaAll(sema(
        "    using var od := new OwningDictionary<int, R>(); "
        "    od.Add(1, new R(1)); "
        "    var se := od.Single(); "
        "    return se.Key; "));
    BEZEL_EXPECT_TRUE(
        hasError(r, "`OwningDictionary<K, V>` does not expose `Single`"));
}

BEZEL_TEST(sema_dict_first_last_elementat_3716,
           owning_dictionary_element_at_rejected_at_sema_4_9) {
    auto r = parseSemaAll(sema(
        "    using var od := new OwningDictionary<int, R>(); "
        "    od.Add(1, new R(1)); "
        "    var ea := od.ElementAt(0); "
        "    return ea.Key; "));
    BEZEL_EXPECT_TRUE(
        hasError(r, "`OwningDictionary<K, V>` does not expose `ElementAt`"));
}

// The reject cites stdlib §4.9's own conformance sentence, so a reader
// (human or Claude) can find the authority without leaving the diagnostic.
BEZEL_TEST(sema_dict_first_last_elementat_3716,
           owning_dictionary_reject_cites_stdlib_4_9) {
    auto r = parseSemaAll(sema(
        "    using var od := new OwningDictionary<int, R>(); "
        "    od.Add(1, new R(1)); "
        "    var fe := od.First(); "
        "    return fe.Key; "));
    BEZEL_EXPECT_TRUE(hasError(r, "stdlib §4.9"));
    BEZEL_EXPECT_TRUE(hasError(r, "IDisposable"));
}
