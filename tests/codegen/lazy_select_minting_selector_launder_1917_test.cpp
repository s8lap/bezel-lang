// Ownership/§28.3 + stdlib §5 (#1917) — a lazy LINQ `Select` whose inline lambda
// selector MINTS a fresh cleanup-bearing value per element launders that value
// out through the projected sequence and leaks it.
//
// ROOT CAUSE (src/ownership/Ownership_Checks.cpp checkLazySelectMintingSelector /
// lazySelectSelectorReturnMints). A lazy `Select<U>(selector: (T) -> U)` returns
// `IEnumerable<U>`, which stdlib §5 ("Ownership at class-typed substitutions",
// v0.194) classifies as a RECEIVER-SOURCED BORROW: the projected `U` is drawn
// from the receiver's elements, so the lazy pipeline (bz_lazy_select) takes NO
// disposal obligation on the projected values — the null element-disposer that is
// correct for a borrowed projection. A selector that instead MINTS a fresh owned
// value per element — `(x) => new Box(x)` (a `new` class), `(w) => w + "!"` /
// `(s) => $"[{s}]"` (a fresh string) — hands that freshly-owned value into the
// borrow-typed sequence with nowhere to be discharged: `ToArray()` owns only the
// CONTAINER, borrowing elements, and no lazy materializer takes ELEMENT
// ownership, so every minted projection (winner and losers) leaks. Codegen cannot
// distinguish a minting projection from a receiver-sourced borrow — disposing on
// drain would double-free the identity / field-borrow cases — so the fix is in
// the ownership analysis: reject the launder at the selector, exactly as the
// §28.3 `Aggregate` minted-SEED launder is rejected (checkBareBorrowTempLaunder)
// and as proposed for the Min/Max minting-selector case (#1915). This is the same
// §28.3 laundering class ("laundering a fresh owned value out through the borrow
// result"), here through the lazy Select's projected sequence.
//
// FIX. The deferred-activation walk (checkDeferredActivationTempSitesExpr, which
// visits every CallExpr in every escaping/consuming position — for-source,
// binding, materialization, terminal) now calls checkLazySelectMintingSelector on
// each call. It recognises a lazy `Select` (a LINQ intrinsic: no user MethodDecl)
// with a single inline-lambda selector whose body mints — producerState == Owned
// (a `new` class / an `owned`-returning call / a `move`), or a fresh string (a
// `+` concatenation or an interpolation) — and rejects it. A borrowing projection
// (the element itself, a field / index read, a value projection) is NOT a mint
// and stays admitted, leak-free, with no double-free.
//
// Fails-without / passes-with: before the fix the minting-selector programs
// compiled CLEAN and leaked their projections under LSan (~16 B/element for a
// `new` class, ~108 B / 6 allocs for `+ "!"` strings). After the fix each is a
// clean COMPILE ERROR (runAot -> diagnostics.hasErrors()); the borrowing/value
// controls still compile and run leak-free under runAotChecked's ASan/LSan net.
//
// Scope (recorded): a string-MINTING method-call selector (`(s) => s.ToUpper()`)
// is NOT rejected — classifying a call's string result as fresh needs
// interprocedural return-kind info the ownership pass does not carry (the same
// recorded gap checkDisposeReceiverOwnership notes); it remains a leak, filed as
// follow-up. The two issue-canonical shapes (`new T(...)`, `w + "!"`) are the
// unambiguous mints this fix closes. Coordinated with #1915 (Min/Max selector) —
// the same laundering class, a separate call site, still open.
//
// Spec: language spec §28.3 *Bare-type-parameter returns* / *Unbound owned
// temporaries* (v0.194 — laundering a fresh owned value out through a borrow
// result), stdlib §5 (deferred LINQ operators; "Ownership at class-typed
// substitutions"). Spec revision v0.197.

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

constexpr std::string_view kBox =
    "class Box "
    "  public V: int; "
    "  public constructor(x: int) V := x; end; "
    "  public method Get(): int return V; end Get; "
    "end Box; ";

}  // namespace

// ===========================================================================
// REJECT — minting selectors (the §28.3 launder). Fail-without / pass-with.
// ===========================================================================

// stdlib §5 / §28.3 (#1917): the issue's canonical repro — `Select(x => new
// Box(x))` enumerated in a for-in mints a fresh owned Box per element that leaks
// (~16 B/element). REJECTED at the selector. Before the fix: compiled clean, leak.
BEZEL_TEST(lazy_select_minting_selector_launder_1917,
           select_new_class_forin_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; " + std::string(kBox) +
        "class C public static method Main(): int "
        "  using var xs: Array<int> := [1, 2, 3]; "
        "  var sum: int := 0; "
        "  for var b in xs.Select(lambda(x) => new Box(x)) do "
        "    sum := sum + b.Get(); "
        "  end for; "
        "  return sum; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// stdlib §5 / §28.3 (#1917): the issue's second canonical repro — `Select(w => w
// + "!")` mints a fresh string per element (~108 B / 6 allocs). REJECTED.
BEZEL_TEST(lazy_select_minting_selector_launder_1917,
           select_string_concat_forin_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => s + \"!\") do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// §28.3 (#1917): the launder is POSITION-INDEPENDENT — a `.ToArray()`
// materialization of a minting `Select` leaks the minted elements just the same
// (ToArray owns only the container, borrowing elements). REJECTED.
BEZEL_TEST(lazy_select_minting_selector_launder_1917,
           select_new_class_toarray_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; " + std::string(kBox) +
        "class C public static method Main(): int "
        "  using var xs: Array<int> := [1, 2, 3]; "
        "  using var arr: Array<Box> := xs.Select(lambda(x) => new Box(x)).ToArray(); "
        "  return arr.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// §28.3 (#1917): binding to an explicit `IEnumerable<Box>` local (the widening
// path) then enumerating leaks identically. REJECTED at the selector.
BEZEL_TEST(lazy_select_minting_selector_launder_1917,
           select_new_class_ienumerable_local_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; " + std::string(kBox) +
        "class C public static method Main(): int "
        "  using var xs: Array<int> := [1, 2, 3]; "
        "  var boxes: IEnumerable<Box> := xs.Select(lambda(x) => new Box(x)); "
        "  var n: int := 0; "
        "  for var b in boxes do n := n + 1; end for; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// §28.3 (#1917): an interpolated-string selector `$"[{s}]"` mints a fresh string
// per element just as `+` does. REJECTED (exercises the interpolation branch).
BEZEL_TEST(lazy_select_minting_selector_launder_1917,
           select_string_interpolation_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\"]; "
        "  var total: int := 0; "
        "  for var w in words.Select(lambda(s) => $\"[{s}]\") do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(compileErrored(src));
    BEZEL_EXPECT_TRUE(saysMintingSelect(src));
}

// ===========================================================================
// ACCEPT (over-reject / double-free guards) — borrowing & value projections.
// These must stay admitted AND run leak-free with no double-free.
// ===========================================================================

// stdlib §5 (#1917): a borrowing selector that projects a CLASS-TYPED borrow —
// `Select(o => o.GetChild())` returning a stored `owned` field reference — is a
// receiver-sourced borrow, NOT a mint. Must stay admitted, and must run leak-free
// with no double-free (the exact case a codegen-dispose approach would corrupt).
// Owning source disposes the outers (and their owned children) exactly once.
// 3 + 4 = 7.
BEZEL_TEST(lazy_select_minting_selector_launder_1917,
           select_class_field_borrow_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class Inner "
        "  public N: int; "
        "  public constructor(n: int) N := n; end; "
        "end Inner; "
        "class Outer "
        "  public owned Child: Inner; "
        "  public constructor(n: int) Child := new Inner(n); end; "
        "  public method GetChild(): Inner return Child; end GetChild; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var xs: OwningArray<Outer> := [new Outer(3), new Outer(4)]; "
        "  var sum: int := 0; "
        "  for var ch in xs.Select(lambda(o) => o.GetChild()) do "
        "    sum := sum + ch.N; "
        "  end for; "
        "  return sum; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}

// stdlib §5 (#1917): a VALUE projection `Select(x => x * 2)` carries no disposal
// obligation and is never a mint — admitted, leak-free. 2 + 4 + 6 = 12.
BEZEL_TEST(lazy_select_minting_selector_launder_1917,
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

// stdlib §5 (#1917): a string IDENTITY projection `Select(s => s)` returns the
// element's own borrowed string — NOT a fresh string — so it is admitted and runs
// leak-free (the array owns and frees each string once). 1 + 2 + 3 = 6.
BEZEL_TEST(lazy_select_minting_selector_launder_1917,
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
