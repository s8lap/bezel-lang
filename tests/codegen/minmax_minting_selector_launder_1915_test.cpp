// Ownership/§28.3 + stdlib §5 (#1915) — a LINQ selector `Min<U>(selector)` /
// `Max<U>(selector)` whose inline lambda selector MINTS a fresh cleanup-bearing
// value per element launders that value out through the aggregation result and
// leaks every projection (winner AND losers).
//
// ROOT CAUSE (src/ownership/Ownership_Checks.cpp checkMinMaxMintingSelector /
// selectorLambdaMintingReturn / lazySelectSelectorReturnMints). After #1901 fixed
// the selector `Min<U>/Max<U>(sel)` result type to `U` (not `int`), the #1876
// carve classifies that `U` at a class/interface substitution as a
// RECEIVER-SOURCED BORROW (stdlib §5, "Ownership at class-typed substitutions",
// v0.194): the pointer-U aggregation path takes NO disposal obligation on any
// projected value — the null-disposer that is correct for a receiver-sourced
// projection (identity / field-borrow). A selector that instead MINTS a fresh
// owned value per element — `(o) => new Box(o)` (a `new` class), `(s) => s + "!"`
// / `(s) => $"[{s}]"` (a fresh string) — hands that freshly-owned value into the
// borrow-typed result: the selector runs on EVERY element to find the extremum,
// so every projection (winner and losers) is freshly owned with nowhere to be
// discharged and leaks (~16 B/element for a `new` class, ~108 B / 6 allocs for
// `+ "!"` strings). Codegen cannot distinguish a minting projection from a
// receiver-sourced borrow — disposing would double-free the identity / field
// cases — so the fix is in the ownership analysis: reject the launder at the
// selector, exactly as the §28.3 `Aggregate` minted-SEED launder is rejected
// (checkBareBorrowTempLaunder) and as the lazy `Select` minting selector is
// rejected (#1917). This is the same §28.3 laundering class ("laundering a fresh
// owned value out through the borrow result"), here through the Min/Max result.
//
// FIX. The deferred-activation walk (checkDeferredActivationTempSitesExpr, which
// visits every CallExpr in every escaping/consuming position — binding, return,
// argument, sub-expression) now calls checkMinMaxMintingSelector on each call. It
// recognises a LINQ `Min`/`Max` (a LINQ intrinsic: no user MethodDecl) with a
// single inline-lambda selector whose body mints — producerState == Owned (a
// `new` class / an `owned`-returning call / a `move`), or a fresh string (a `+`
// concatenation or an interpolation) — and rejects it, sharing the selector-body
// walk (selectorLambdaMintingReturn) and the mint predicate
// (lazySelectSelectorReturnMints) with the #1917 `Select` reject. A borrowing
// projection (the element itself, a field / index read, a value projection) is
// NOT a mint and stays admitted, leak-free, with no double-free.
//
// Fails-without / passes-with: before the fix the minting-selector programs
// compiled CLEAN and leaked their projections under LSan (48 B / 3 `Box` for the
// canonical repro, 108 B / 6 allocs for `+ "!"` strings). After the fix each is a
// clean COMPILE ERROR (runAot -> diagnostics.hasErrors()); the borrowing/value
// controls still compile and run leak-free under runAotChecked's ASan/LSan net.
//
// Scope (recorded, shared with #1917): a string-MINTING method-call selector
// (`(s) => s.ToUpper()`) and a block body that returns a fresh value via an
// intermediate owned local (`(o) => do var b := new Box(o); return b; end`) are
// NOT rejected — classifying those as fresh needs interprocedural return-kind /
// local-ownership info the ownership pass's producerState does not carry (the same
// recorded gap as #1917). The issue-canonical inline shapes (`new T(...)`,
// `s + "!"`, an interpolation, and a block body returning the producer directly)
// are the unambiguous mints this fix closes.
//
// Spec: language spec §28.3 *Bare-type-parameter returns* (v0.194 — laundering a
// fresh owned value out through a borrow result), stdlib §5 (the selector
// `Min<U>`/`Max<U>` aggregations; "Ownership at class-typed substitutions").

#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A compile-time error was reported (the §28.3 minting Min/Max selector reject).
bool compileErrored(const std::string& src) {
    return runAot(src).diagnostics.hasErrors();
}

// The specific §28.3/§5 minting-Min/Max-selector launder diagnostic
// (distinguishes the intended reject from any incidental error).
bool saysMintingMinMax(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`Min`/`Max` selector that mints") !=
            std::string::npos)
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

// A `Box` that is `IComparable<Box>` so `Min<Box>`/`Max<Box>` typecheck.
constexpr std::string_view kBox =
    "class Box: IComparable<Box> "
    "  public V: int; "
    "  public constructor(x: int) V := x; end; "
    "  public method Get(): int return V; end Get; "
    "  public method CompareTo(other: Box): int return V - other.V; "
    "  end CompareTo; "
    "end Box; ";

}  // namespace

// ===========================================================================
// REJECT — minting selectors (the §28.3 launder). Fail-without / pass-with.
// ===========================================================================

// stdlib §5 / §28.3 (#1915): the issue's canonical repro — `Max(o => new Box(o))`
// mints a fresh owned Box per element; the winner is returned as a borrow and
// every minted projection (winner and losers) leaks (48 B / 3 Box). REJECTED at
// the selector. Before the fix: compiled clean, leaked.
BEZEL_TEST(minmax_minting_selector_launder_1915,
           max_new_class_binding_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; " + std::string(kBox) +
        "class C public static method Main(): int "
        "  using var xs: Array<int> := [1, 2, 3]; "
        "  var winner: Box := xs.Max((o) => new Box(o)); "
        "  return winner.Get(); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(compileErrored(src));
    BEZEL_EXPECT_TRUE(saysMintingMinMax(src));
}

// stdlib §5 / §28.3 (#1915): the `Min` variant of the canonical repro — the same
// laundering class through the Min aggregation. REJECTED.
BEZEL_TEST(minmax_minting_selector_launder_1915,
           min_new_class_binding_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; " + std::string(kBox) +
        "class C public static method Main(): int "
        "  using var xs: Array<int> := [1, 2, 3]; "
        "  var winner: Box := xs.Min((o) => new Box(o)); "
        "  return winner.Get(); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingMinMax(src));
}

// stdlib §5 / §28.3 (#1915): the issue's second canonical shape — `Max(s => s +
// "!")` mints a fresh string per element (~108 B / 6 allocs). REJECTED.
BEZEL_TEST(minmax_minting_selector_launder_1915,
           max_string_concat_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "  var w: string := words.Max((s) => s + \"!\"); "
        "  return w.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingMinMax(src));
}

// §28.3 (#1915): an interpolated-string selector `$"[{s}]"` mints a fresh string
// per element just as `+` does. REJECTED (exercises the interpolation branch of
// the shared lazySelectSelectorReturnMints predicate).
BEZEL_TEST(minmax_minting_selector_launder_1915,
           max_string_interpolation_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\"]; "
        "  var w: string := words.Max((s) => $\"[{s}]\"); "
        "  return w.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(compileErrored(src));
    BEZEL_EXPECT_TRUE(saysMintingMinMax(src));
}

// §28.3 (#1915): the launder is POSITION-INDEPENDENT — a minting `Max` in a
// RETURN position leaks its projections just the same. REJECTED at the selector.
BEZEL_TEST(minmax_minting_selector_launder_1915,
           max_new_class_return_position_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; " + std::string(kBox) +
        "class C "
        "  public static method Pick(xs: Array<int>): int "
        "    return xs.Max((o) => new Box(o)).Get(); "
        "  end Pick; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [1, 2, 3]; "
        "    return Pick(xs); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(saysMintingMinMax(src));
}

// §28.3 (#1915): a block-body selector that returns the producer DIRECTLY
// (`do return new Box(o); end`) is a mint just as the `=> new Box(o)` form is —
// the shared selector-body walk visits the `return` statement. REJECTED.
BEZEL_TEST(minmax_minting_selector_launder_1915,
           max_new_class_block_body_direct_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Linq; " + std::string(kBox) +
        "class C public static method Main(): int "
        "  using var xs: Array<int> := [1, 2, 3]; "
        "  var winner: Box := xs.Max((o) => do return new Box(o); end); "
        "  return winner.Get(); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingMinMax(src));
}

// ===========================================================================
// ACCEPT (over-reject / double-free guards) — borrowing & value projections.
// These must stay admitted AND run leak-free with no double-free.
// ===========================================================================

// stdlib §5 (#1915): a borrowing selector that projects a CLASS-TYPED borrow —
// `Max(o => o.GetChild())` returning a stored `owned` field reference — is a
// receiver-sourced borrow, NOT a mint. Must stay admitted, and must run leak-free
// with no double-free (the exact case a codegen-dispose approach would corrupt).
// The owning source disposes the outers (and their owned children) exactly once;
// the returned Inner borrow is valid while the source is live. Max by N = 4.
BEZEL_TEST(minmax_minting_selector_launder_1915,
           max_class_field_borrow_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class Inner: IComparable<Inner> "
        "  public N: int; "
        "  public constructor(n: int) N := n; end; "
        "  public method CompareTo(other: Inner): int return N - other.N; "
        "  end CompareTo; "
        "end Inner; "
        "class Outer "
        "  public owned Child: Inner; "
        "  public constructor(n: int) Child := new Inner(n); end; "
        "  public method GetChild(): Inner return Child; end GetChild; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var xs: OwningArray<Outer> := [new Outer(3), new Outer(4)]; "
        "  var best: Inner := xs.Max((o) => o.GetChild()); "
        "  return best.N; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 4);
}

// stdlib §5 (#1915): a VALUE projection `Max(o => o * 2)` carries no disposal
// obligation and is never a mint — admitted, leak-free. max(2,4,6) = 6.
BEZEL_TEST(minmax_minting_selector_launder_1915,
           max_value_projection_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs: Array<int> := [1, 2, 3]; "
        "  var m: int := xs.Max((o) => o * 2); "
        "  return m; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 6);
}

// stdlib §5 (#1915): an identity value projection `Max(o => o)` is not a mint —
// admitted, leak-free. max(1,2,3) = 3.
BEZEL_TEST(minmax_minting_selector_launder_1915,
           max_identity_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs: Array<int> := [1, 2, 3]; "
        "  var m: int := xs.Max((o) => o); "
        "  return m; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}

// stdlib §5 (#1915): a string IDENTITY projection `Max(s => s)` returns the
// element's own borrowed string — NOT a fresh string — so it is admitted and runs
// leak-free (the array owns and frees each string once). max = "ccc", Length 3.
BEZEL_TEST(minmax_minting_selector_launder_1915,
           max_string_identity_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "  var w: string := words.Max((s) => s); "
        "  return w.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}

// §2.2 / §28.3 (#1915): `Math.Max(a, b)` (a value-argument selector, no lambda,
// two args) must NOT be caught by the Min/Max minting-selector reject — the
// loop-carried accumulation `prev := Math.Max(prev, cur)` is a standard, sound
// pattern. Admitted, leak-free. max over [3,1,4,1,5] = 5.
BEZEL_TEST(minmax_minting_selector_launder_1915,
           math_max_accumulation_accepted_leakfree_spec_2_2) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  var prev: int := 0; "
        "  for var cur in [3, 1, 4, 1, 5] do "
        "    prev := Math.Max(prev, cur); "
        "  end for; "
        "  return prev; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 5);
}
