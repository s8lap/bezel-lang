// Ownership/§28.3 + stdlib §5 (#7201) — a lazy LINQ `Zip<U,R>(other, combine)`
// whose COMBINER MINTS a fresh cleanup-bearing value per pair launders that
// value out through the zipped sequence and leaks every minted combination.
//
// ROOT CAUSE (src/ownership/Ownership_Checks.cpp checkLazyZipMintingCombiner).
// stdlib §5 declares `public method Zip<U, R>(other: IEnumerable<U>, combine:
// (T, U) -> R): IEnumerable<R>;` — the combined element `R` is the COMBINER's
// result, and the operator returns a BORROWING `IEnumerable<R>`. Under stdlib
// §5's "Ownership at class-typed substitutions" (v0.194) the projected element
// of a lazy operator is a RECEIVER-SOURCED BORROW: the pipeline takes NO
// disposal obligation on it, and no lazy materializer takes ELEMENT ownership
// (`ToArray` owns only the CONTAINER, borrowing elements). A combiner that
// MINTS — a `+` concatenation, an interpolation, a `new` of a class, an
// `owned`-returning call — produces a fresh owned value per pair with nowhere
// to be discharged, so every minted combination leaks.
//
// MEASURED on the staging tip (the R35 #7160 reviewer's arms), under
// `bezelc --sanitize=address`:
//
//   SUMMARY: AddressSanitizer: 72 byte(s) leaked in 4 allocation(s).
//   Direct leak of 64 byte(s) in 2 object(s) allocated from:
//       #1 ... in lam$0            (the produced string's header, 32 B each)
//   Indirect leak of 8 byte(s) in 2 object(s) allocated from:
//       #1 ... in lam$0            (the produced string's data, 3 B + 5 B)
//
// ⇒ the 72 B is the two produced STRINGS — the combiner's closure cell is NOT
// leaked (the lazy arms own and release it, `bz_lazy_own_closure`). The leak
// is spelling-independent: the inline-lambda combiner and the method-reference
// combiner (`Cat`) leak the SAME 72 B, and so does the `ToArray` materializer
// (the drain's `bz_array_retain_strings` adds a second reference the array
// releases again, leaving the pipeline's share unreleased).
//
// WHY A REJECT AND NOT A RUNTIME RELEASE. The compiler already refuses the
// sibling shapes: a lazy `Select` whose selector mints (#1917), a `Min`/`Max`
// selector that mints (#1915), and an `Aggregate` minted seed. Each is the
// same §28.3 laundering class ("laundering a fresh owned value out through a
// borrow result"), and each is refused AT THE CALLBACK rather than discharged
// at run time — because there is no run-time owner: the pipeline's elements
// are borrows, and a consumer that materializes them re-borrows them. The
// `Zip` combiner is the same class and was the one §5 callback-taking operator
// with NO gate arm (the `SelectMany` arm is #7184's separate open issue).
//
// FIX. The deferred-activation walk (checkDeferredActivationTempSitesExpr,
// which visits every CallExpr in every escaping/consuming position) now calls
// checkLazyZipMintingCombiner on each call. It recognises a LINQ `Zip` (a
// LINQ intrinsic: no user MethodDecl) whose SECOND argument — the combiner —
// is a callback that MINTS: an inline lambda (the shared
// selectorLambdaMintingReturn walk) or a METHOD GROUP whose own body mints
// (the callee's return paths, via callbackMethodMintingReturn). A borrowing
// combiner (the element itself, a field read, a value-typed combiner) is NOT a
// mint and stays admitted, leak-free, with no double-free.
//
// Fails-without / passes-with: before the fix the minting-combiner programs
// compiled CLEAN and leaked their combinations under LSan (72 B / 4 allocations
// for the canonical two-element repro). After the fix each is a clean COMPILE
// ERROR (runAot -> diagnostics.hasErrors()); the borrowing / value / empty
// controls still compile and run leak-free under runAotChecked's ASan/LSan net.
//
// Scope (recorded, shared with #1917 / #1915): a combiner whose mint is
// INDIRECTED through an intermediate local (`do var t := a + b; return t; end`)
// is not rejected — classifying that needs the ownership pass's local-ownership
// info it does not carry (the same recorded gap as #1917). An IMPORTED
// method-reference combiner has no AST body here, so it stays admitted (the
// "cannot prove it mints" direction). The issue-canonical inline shapes (`+`
// concatenation, an interpolation, a `new` class, and a method reference whose
// own body returns the producer directly) are the unambiguous mints this fix
// closes.
//
// Spec: language spec §28.3 *Bare-type-parameter returns* (v0.194 — laundering
// a fresh owned value out through a borrow result), §29.11 *LINQ pipelines*
// (the projected element is classified as a receiver-sourced borrow). Stdlib
// §5 "Ownership at class-typed substitutions" (v0.194) and the `Zip<U,R>`
// signature.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A compile-time error was reported (the §28.3 minting-Zip combiner reject).
bool compileErrored(const std::string& src) {
    return runAot(src).diagnostics.hasErrors();
}

// The specific §28.3/§5 minting-Zip-combiner launder diagnostic
// (distinguishes the intended reject from any incidental error).
bool saysMintingZip(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`Zip` combiner that mints") != std::string::npos)
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

// A `Box` the class-producing combiner can mint.
constexpr std::string_view kBox =
    "class Box "
    "  public V: string; "
    "  public constructor(v: string) V := v; end; "
    "end Box; ";

}  // namespace

// ===========================================================================
// REJECT — minting combiners (the §28.3 launder). Fail-without / pass-with.
// ===========================================================================

// stdlib §5 / §28.3 (#7201): the issue's canonical repro — the inline-lambda
// combiner `lambda(a, b) => a + b` mints a fresh string per pair; the two
// pairs leaked 72 B in 4 allocations. REJECTED at the combiner.
// Before the fix: compiled clean, leaked.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_lambda_string_concat_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Zip(ys, lambda(a, b) => a + b) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(compileErrored(src));
    BEZEL_EXPECT_TRUE(saysMintingZip(src));
}

// stdlib §5 / §28.3 (#7201): the METHOD-REFERENCE spelling of the same
// combiner — `Cat(a, b) => a + b` — leaks the same 72 B (the produced strings
// are allocated in the callee). REJECTED at the callee's return path.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_method_reference_string_concat_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C "
        "  private static method Cat(a: string, b: string): string => a + b; "
        "  public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Zip(ys, Cat) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(compileErrored(src));
    BEZEL_EXPECT_TRUE(saysMintingZip(src));
}

// stdlib §5 / §28.3 (#7201): the QUALIFIED method-reference spelling
// (`C.Cat`) is the same laundering class. REJECTED.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_qualified_method_reference_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C "
        "  private static method Cat(a: string, b: string): string => a + b; "
        "  public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Zip(ys, C.Cat) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingZip(src));
}

// §28.3 (#7201): a class-producing combiner `lambda(a, b) => new Box(a + b)`
// mints a fresh OWNED class per pair — the same launder, and the shape the
// #1917 `Select` gate refuses. REJECTED.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_new_class_combiner_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; " +
        std::string(kBox) +
        "class C public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Zip(ys, lambda(a, b) => new Box(a + b)) do "
        "    total := total + w.V.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(compileErrored(src));
    BEZEL_EXPECT_TRUE(saysMintingZip(src));
}

// §28.3 (#7201): an interpolating combiner `lambda(a, b) => $"[{a}{b}]"` mints
// a fresh string per pair just as `+` does (the interpolation branch of the
// shared mint predicate). REJECTED.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_string_interpolation_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Zip(ys, lambda(a, b) => $\"[{a}{b}]\") do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(compileErrored(src));
    BEZEL_EXPECT_TRUE(saysMintingZip(src));
}

// §28.3 (#7201): the launder is POSITION-INDEPENDENT — a minting `Zip` bound
// to a local (not enumerated in a for-loop) leaks its combinations just the
// same, so the reject fires in a binding position too.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_binding_position_rejected_spec_28_3) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var z: IEnumerable<string> := xs.Zip(ys, lambda(a, b) => a + b); "
        "  var total: int := 0; "
        "  for var w in z do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(saysMintingZip(src));
}

// ===========================================================================
// ACCEPT (over-reject / double-free guards) — borrowing & value combiners.
// These must stay admitted AND run leak-free with no double-free.
// ===========================================================================

// stdlib §5 (#7201): a VALUE combiner `lambda(a, b) => a.Length + b.Length`
// carries no disposal obligation and is never a mint — admitted, leak-free.
// "a"+"bb" vs "c"+"dd": (1+2) + (1+2) = 6.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_int_result_combiner_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Zip(ys, lambda(a, b) => a.Length + b.Length) do "
        "    total := total + w; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 6);
}

// stdlib §5 (#7201): a BORROWING combiner `lambda(a, b) => a` returns the
// left element's own string — NOT a fresh string — so it is admitted and runs
// leak-free (the source array owns and frees each string once). Lengths
// 1 + 2 = 3.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_string_borrow_combiner_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Zip(ys, lambda(a, b) => a) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}

// stdlib §5 (#7201): the METHOD-REFERENCE spelling of a BORROWING combiner
// (`First(a, b) => a`) is admitted too — the callee's own return path decides,
// and it borrows. Leak-free. Lengths 1 + 2 = 3.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_method_reference_borrow_combiner_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C "
        "  private static method First(a: string, b: string): string => a; "
        "  public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Zip(ys, First) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}

// stdlib §5 (#7201): an EMPTY source produces no pair at all — the combiner
// never runs, so nothing is minted and nothing is released twice. Admitted,
// leak-free, total 0.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_empty_source_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs: Array<string> := []; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Zip(ys, lambda(a, b) => a) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// stdlib §5 / §29.4 (#7160, #7201): the sibling shape the #7160 fix pinned —
// a method-reference combiner with an `int` result — must stay admitted and
// leak-free (the gate rejects a MINT, never a value result). Two pairs, each
// combining to 1.
//
// NOTE the bare-method-reference spelling types the Zip's element as the
// SOURCE element type (sema carries no Function TypeRef for a bare method
// group — #4897/#7160), so the loop variable is read as the source `string`
// and the row counts the pairs rather than the combined values; that is the
// same shape and the same expectation as the #7160 row.
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           zip_method_reference_int_result_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C "
        "  private static method Pick(a: string, b: string): int => 1; "
        "  public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  using var ys: Array<string> := [\"c\", \"dd\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Zip(ys, Pick) do "
        "    total := total + 1; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 2);
}

// stdlib §5 (#7201): the SIBLING gate arms must not regress — a lazy `Select`
// whose selector projects a BORROW and a `Where` whose predicate reads a field
// stay admitted and leak-free (this is the #1917 gate's own admitted surface,
// one operator upstream of the new `Zip` arm).
BEZEL_TEST(lazy_zip_minting_combiner_launder_7201,
           select_where_sibling_arms_accepted_leakfree_spec_5) {
    std::string src =
        "module M; import System.Collections.Generic; import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs: Array<string> := [\"a\", \"bb\"]; "
        "  var total: int := 0; "
        "  for var w in xs.Where(lambda(x) => x.Length > 0).Select(lambda(x) => x) do "
        "    total := total + w.Length; "
        "  end for; "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 3);
}
