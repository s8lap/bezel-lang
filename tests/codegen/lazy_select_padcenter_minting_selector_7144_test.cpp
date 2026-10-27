// Ownership/§28.3 + stdlib §9.6 (#7144) — §28.3's lazy-`Select` MINTING-SELECTOR
// gate did not know `string.PadCenter`, so `Select(x => x.PadCenter(5))` was
// ACCEPTED and LEAKED.
//
// ROOT CAUSE (src/ownership/Ownership_Checks.cpp
// MethodOwnership::isStdlibStringMintingCall). The §28.3 lazy-`Select` gate
// rejects a selector whose body MINTS a fresh owned value per element: stdlib
// §5 classifies `Select`'s projected element as a RECEIVER-SOURCED BORROW, so
// the lazy pipeline takes no disposal obligation on the projection and every
// minted projection leaks. The gate recognises a string-minting selector
// through a NAME list of the stdlib §9.6 members whose contract is "returns a
// new `string` value; original unchanged" — the list #1931 introduced, frozen
// at stdlib §9.6 v0.197.
//
// The #7133 fix gave stdlib §9.6's `PadCenter` its lowering (the
// `bz_str_pad` runtime helper, a FRESH allocation per call) without adding it
// to that list, so the gate did not fire for it. `PadCenter` is a mint by
// §9.6's own contract —
//
//   public pure method PadCenter(totalWidth: int, paddingChar: char := ' '): string;
//   // PadCenter splits the padding evenly; when it is odd, the extra
//   // character goes to the RIGHT: "ab".PadCenter(5) = " ab  ".
//
// — the same "returns a new `string` value; original unchanged" contract its
// siblings `PadLeft` / `PadRight` carry, and the same one the gate already
// enforced for them.
//
// FIX. `PadCenter` joins the documented-mint set. The set is now DERIVED from
// §9.6's member list: every §9.6 INSTANCE member whose declared result is
// `string` and whose contract is "returns a new `string` value; original
// unchanged" — every such member EXCEPT `ToString()` (§9.6: "returns this" —
// a BORROW of the receiver) and the `AsSpan` family (declared result
// `ReadOnlyStringSpan`, not `string`).
//
// Fails-without / passes-with: before the fix `Select(x => x.PadCenter(5))`
// compiled CLEAN and leaked (MEASURED under LSan: 114 B in 6 allocations for
// three elements — 96 B direct + 18 B indirect, all from `bz_str_pad`), while
// its siblings `PadLeft` / `PadRight` were REJECTED by the same gate. After
// the fix the `PadCenter` selectors are a clean COMPILE ERROR carrying the
// §28.3 minting-selector diagnostic, and the BORROWED selectors (element /
// field / index read, and a user-defined method returning a borrowed string)
// stay ADMITTED and run leak-free.
//
// OVER-REJECTION GUARD (both directions measured). The gate must keep
// admitting a selector that returns a BORROWED string: a bare element read, a
// field read of a `string`, an index read of a `string` element, and a
// user-defined method returning a borrowed string. The no-`MethodDecl` gate in
// `isStdlibStringMintingCall` (a user override may return a borrow) is what
// keeps the last of those admitted, and it must survive this change.
//
// Scope (recorded): `GetField` and the INSTANCE `Repeat` are implemented by
// the compiler but are NOT declared by stdlib §9.6 — that is #7131 (a
// `spec-defect` issue), and their place in the mint set is #7131's decision.
// This commit does NOT change them: `Select(x => x.GetField(',', 0))` and
// `Select(x => x.Repeat(3))` are still ACCEPTED here, exactly as before.
//
// Spec: language spec §28.3 *Bare-type-parameter returns* / *Unbound owned
// temporaries* (v0.194 — laundering a fresh owned value out through a borrow
// result), stdlib §5 "Ownership at class-typed substitutions" (v0.194 — the
// projected element is a receiver-sourced borrow), stdlib §9.6 `String`
// (v0.343 — `PadCenter`'s declared contract and the "returns a new `string`
// value; original unchanged" modification family). Spec revision v0.343.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The specific §28.3 minting-Select diagnostic (distinguishes the intended
// reject from any incidental error).
bool saysMintingSelect(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`Select` selector that mints") != std::string::npos)
            return true;
    }
    return false;
}

// The §28.3 minting-selector diagnostic for the `Min` / `Max` aggregation
// family (the sibling gate #1915 opened — its own wording).
bool saysMintingMinMax(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`Min`/`Max` selector that mints") != std::string::npos)
            return true;
    }
    return false;
}

// runAotChecked compiles + links + runs under generated-code ASan/LSan; a leak,
// double-free, or crash flips r.ran to false -> -999 (never a legal exit value).
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// REJECT — the `PadCenter` minting selectors (§28.3 / stdlib §9.6, #7144).
// Fail-without: before the fix each of these compiled CLEAN and leaked.
// ===========================================================================

// stdlib §9.6 (v0.343) / §28.3 (#7144): the issue's canonical repro —
// `Select(x => x.PadCenter(5))` mints a fresh string per element (MEASURED
// under LSan before the fix: 114 B in 6 allocations for three elements).
// REJECTED at the selector, exactly like its `PadLeft` sibling.
BEZEL_TEST(lazy_select_padcenter_minting_selector_7144,
           select_string_padcenter_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s.PadCenter(5)) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// stdlib §9.6 (v0.343) / §28.3 (#7144): `Min` shares the §28.3
// minting-selector gate with `Select` (the same gate #1915 opened for
// `Min` / `Max`), so `Min(lambda(x) => x.PadCenter(5))` is REJECTED too.
BEZEL_TEST(lazy_select_padcenter_minting_selector_7144,
           min_string_padcenter_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\"]; "
        "  var best: string := words.Min(lambda(s) => s.PadCenter(5)); "
        "  return best.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingMinMax(src));
}

// stdlib §9.6 (v0.343) / §28.3 (#7144): the reject is POSITION-INDEPENDENT —
// a `.ToArray()` materialization of a `PadCenter` selector mints identically.
// REJECTED at the selector (the deferred-activation walk visits every CallExpr
// in every escaping / consuming position).
BEZEL_TEST(lazy_select_padcenter_minting_selector_7144,
           select_string_padcenter_toarray_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\"]; "
        "  using var arr: Array<string> := "
        "    words.Select(lambda(s) => s.PadCenter(5)).ToArray(); "
        "  return arr.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// stdlib §9.6 (v0.343) / §28.3 (#7144): the 2-argument `PadCenter` spelling
// (`PadCenter(totalWidth, paddingChar)`) is the same member and the same mint
// — REJECTED.
BEZEL_TEST(lazy_select_padcenter_minting_selector_7144,
           select_string_padcenter_with_char_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s.PadCenter(5, '.')) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// ===========================================================================
// ACCEPT (over-rejection guard) — BORROWED selectors must stay admitted and
// run leak-free. Both directions of the gate are measured here.
// ===========================================================================

// stdlib §5 (v0.194) / §28.3 (#7144): the borrowing selector `Select(s => s)`
// (the element itself) is NOT a mint — it stays admitted and leak-free.
// 1 + 2 + 3 = 6.
BEZEL_TEST(lazy_select_padcenter_minting_selector_7144,
           select_string_identity_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s) do total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 6);
}

// stdlib §5 (v0.194) / §28.3 (#7144): an INDEX read of a `string` element
// (`words[i]`) is a BORROW of the element, not a mint — admitted, leak-free.
// 1 + 2 + 3 = 6.
BEZEL_TEST(lazy_select_padcenter_minting_selector_7144,
           select_string_index_read_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "  using var idx: Array<int> := [0, 1, 2]; "
        "  var total: int := 0; "
        "  for var w in idx.Select(lambda(i) => words[i]) do total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 6);
}

// stdlib §5 (v0.194) / §28.3 (#7144): a FIELD read of a `string` is a BORROW
// of the receiver's slot — admitted, leak-free. "a" + "bb" = 3.
BEZEL_TEST(lazy_select_padcenter_minting_selector_7144,
           select_string_field_read_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "record R(Name: string); "
        "class C public static method Main(): int "
        "  using var rows: Array<R> := [new R(\"a\"), new R(\"bb\")]; "
        "  var total: int := 0; "
        "  for var w in rows.Select(lambda(r) => r.Name) do total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}

// stdlib §5 (v0.194) / §28.3 (#7144): a USER-DEFINED method returning a
// BORROWED string is left alone by the gate (the no-`MethodDecl` gate in
// `isStdlibStringMintingCall`) — admitted, leak-free. 1 + 2 = 3.
BEZEL_TEST(lazy_select_padcenter_minting_selector_7144,
           select_user_method_borrowed_string_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method Identity(s: string): string => s; "
        "  public static method Main(): int "
        "    using var words: Array<string> := [\"a\", \"bb\"]; "
        "    var total: int := 0; "
        "    for var w in words.Select(lambda(s) => Identity(s)) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}

// stdlib §9.6 (v0.343) / §28.3 (#7144): `ToString()` is §9.6's ONE instance
// member whose result is a `string` but which does NOT mint — §9.6 says it
// "returns this". It is therefore NOT in the documented-mint set, and a
// `ToString` selector stays ADMITTED (it borrows the receiver) and leak-free.
// 1 + 2 = 3.
BEZEL_TEST(lazy_select_padcenter_minting_selector_7144,
           select_string_tostring_accepted_leakfree_spec_9_6) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s.ToString()) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}
