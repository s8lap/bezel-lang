// Ownership/§28.3 + stdlib §5 + §27 (#1931) — a lazy LINQ `Select` whose inline
// lambda selector MINTS a fresh cleanup-bearing value per element via a STDLIB
// METHOD CALL (e.g. `s.ToUpper()`, `s.Substring(0, 2)`) launders that value out
// through the projected sequence and leaks it.
//
// ROOT CAUSE (src/ownership/Ownership_Checks.cpp lazySelectSelectorReturnMints).
// The #1917 fix (checkLazySelectMintingSelector) closed the analogous gap for
// STRUCTURALLY UNAMBIGUOUS minting shapes — a `new` class, an `owned`-returning
// call, a `move`, or a fresh string from `+` / interpolation. Classifying a
// CALL RESULT as a freshly-minted string needs interprocedural return-kind info
// the ownership pass does not carry: a `s.ToUpper()` call IS a string-returning
// call (no `owned` marker — strings are primitives, not disposable), but the
// stdlib string-minting methods (`ToUpper`/`ToLower`/`Substring`/`Trim`/
// `Replace`/`Insert`/`Remove`/`PadLeft`/`PadRight`/slice indexing) overwhelmingly
// allocate a fresh string. A user method returning a *borrowed* string is also
// possible, so the fix is targeted: reject only when the callee is a stdlib
// method (no user MethodDecl) whose NAME is in the well-known string-minting
// set AND whose result is a string. The #1917 / #1915 reject already runs on
// this walk; #1931 extends the inner detection predicate.
//
// FIX. `lazySelectSelectorReturnMints` is extended with a string-METHOD-CALL
// mint branch: it (a) sees a `CallExpr`, (b) is the receiver a string (or a
// static string-class call), (c) has NO user MethodDecl (so it is the stdlib
// method, never a user override that may return a borrow), and (d) has a name
// in the documented set { ToUpper, ToLower, Substring, Trim, TrimStart,
// TrimEnd, Replace, PadLeft, PadRight, Insert, Remove, Repeat, Join }. A bare
// element / field read (`s` / `s.Length` / `s[0]`) is still NOT a mint and stays
// admitted; a user method call is left alone. The reject reuses the existing
// `Select`-specific diagnostic so the user is steered to the same fix
// directions as #1917 (project a borrow of the element or build an owning
// collection).
//
// Fails-without / passes-with: before the fix the method-call-minting programs
// compiled CLEAN and leaked under LSan (~96 B direct / ~9 B indirect per
// element, ~6 allocations total). After the fix each is a clean COMPILE ERROR
// (runAot -> diagnostics.hasErrors()); the borrowing / value / identity
// controls still compile and run leak-free under runAotChecked's ASan/LSan
// oracle.
//
// Scope (recorded): a NON-stdlib string-returning call selector
// (`Select(s => User.MyMethod(s))` over a user method that happens to return
// a borrowed string) is NOT rejected — the user-method gate keeps it open
// because a user override can return a borrow and the ownership pass cannot
// classify it. That is the same over-rejection concern #1917 / #1915 record;
// the documented-mint set is the unambiguous-sound-rejection surface. Likewise
// a method-reference selector `Select(StaticMint)` is left to the
// method-reference follow-up (shared with #1917 / #1915). The fix mirrors the
// shared `selectorLambdaMintingReturn` helper.
//
// Spec: language spec §28.3 *Bare-type-parameter returns* / *Unbound owned
// temporaries* (v0.194 — laundering a fresh owned value out through a borrow
// result), §27 *Closures* (capture-by-borrow vs owned-transfer semantics for
// the inline-lambda selector), §29.11 *LINQ pipelines* (deferred
// materialization; the projected element is classified as receiver-sourced
// borrow at a class/interface substitution). Stdlib §5 "Ownership at class-
// typed substitutions" (v0.194) / §9.6 `String` (v0.197 — string-minting
// methods).

#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A compile-time error was reported (the §28.3 minting-Select launder reject).
bool compileErrored(const std::string& src) {
    return runAot(src).diagnostics.hasErrors();
}

// The specific §28.3/§5 minting-Select launder diagnostic (distinguishes the
// intended reject from any incidental error).
bool saysMintingSelect(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`Select` selector that mints") != std::string::npos)
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
// REJECT — method-call minting selectors (the §28.3 launder, #1931 extension).
// Fail-without / pass-with.
// ===========================================================================

// stdlib §5 / §28.3 (#1931): the issue's canonical repro —
// `Select(s => s.ToUpper())` mints a fresh string per element (~96 B direct +
// ~9 B indirect per element, ~6 allocations). REJECTED at the selector.
// Before the fix: compiled clean, leak.
BEZEL_TEST(lazy_select_method_call_minting_selector_leak_1931,
           select_string_toupper_forin_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s.ToUpper()) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// §28.3 (#1931): `Select(s => s.ToLower())` — same laundering class. REJECTED.
BEZEL_TEST(lazy_select_method_call_minting_selector_leak_1931,
           select_string_tolower_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"A\", \"BB\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s.ToLower()) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// §28.3 (#1931): `Select(s => s.Substring(0, 2))` — Substring is a string-
// minting stdlib method. REJECTED.
BEZEL_TEST(lazy_select_method_call_minting_selector_leak_1931,
           select_string_substring_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"hello\", \"world\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s.Substring(0, 2)) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// §28.3 (#1931): `Select(s => s.Trim())` — Trim is a string-minting stdlib
// method. REJECTED.
BEZEL_TEST(lazy_select_method_call_minting_selector_leak_1931,
           select_string_trim_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"  a  \", \" bb \"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s.Trim()) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// §28.3 (#1931): `Select(s => s.Replace(\"a\", \"b\"))` — Replace is a string-
// minting stdlib method. REJECTED.
BEZEL_TEST(lazy_select_method_call_minting_selector_leak_1931,
           select_string_replace_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"aaa\", \"baa\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s.Replace(\"a\", \"b\")) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// §28.3 (#1931): the launder is POSITION-INDEPENDENT — a `.ToArray()`
// materialization of a method-call-minting Select leaks identically. REJECTED
// at the selector (the deferred-activation walk visits every CallExpr in every
// escaping/consuming position).
BEZEL_TEST(lazy_select_method_call_minting_selector_leak_1931,
           select_string_toupper_toarray_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\"]; "
        "  using var arr: Array<string> := "
        "    words.Select(lambda(s) => s.ToUpper()).ToArray(); "
        "  return arr.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// ===========================================================================
// ACCEPT (over-reject / double-free guards) — borrowing & value projections.
// These must stay admitted AND run leak-free with no double-free.
// ===========================================================================

// stdlib §5 (#1931): the borrowing selector `Select(s => s)` (identity) is NOT
// a mint — must stay admitted, leak-free. 1 + 2 + 3 = 6.
BEZEL_TEST(lazy_select_method_call_minting_selector_leak_1931,
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

// stdlib §5 (#1931): a value projection `Select(x => x * 2)` carries no
// disposal obligation. Admitted, leak-free. 2 + 4 + 6 = 12.
BEZEL_TEST(lazy_select_method_call_minting_selector_leak_1931,
           select_value_projection_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs: Array<int> := [1, 2, 3]; "
        "  var sum: int := 0; "
        "  for var y in xs.Select(lambda(x) => x * 2) do sum := sum + y; end for; "
        "  return sum; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 12);
}

// stdlib §5 (#1931): `Select(s => s.Length)` projects a borrowed INT (a
// string PROPERTY read) — NOT a mint, not a string. Admitted, leak-free.
// 1 + 2 + 3 = 6.
BEZEL_TEST(lazy_select_method_call_minting_selector_leak_1931,
           select_string_length_property_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "  var total: int := 0; "
        "  for var n in words.Select(lambda(s) => s.Length) do "
        "    total := total + n; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 6);
}
