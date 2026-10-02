// fix #1693 / §15.4 (stdlib §5 SelectMany): a collection literal as the
// expression body of a LINQ selector lambda.
//
// Repro (issue #1693, probe p9_selectmany):
//
//     var xs: Array<int> := [1, 2];
//     var ys := xs.SelectMany((x) => [x, x + 10]);   // <-- literal body
//     for var y in ys do Console.WriteLine(y); end;  // printed NOTHING
//
// The selector's declared return type is `IEnumerable<U>` (stdlib §5:
// `SelectMany<U>(selector: (T) -> IEnumerable<U>): IEnumerable<U>`). Per §15.4 a
// collection literal has NO standalone type — it is well-typed only against a
// determinable, *concrete* target from a closed list (an inline array, or
// Array/OwningArray, Set, Stack, Queue, LinkedList, Dictionary and their owning
// forms, or a `FromCollectionLiteral` type). A collection INTERFACE
// (`IEnumerable<U>`) is deliberately NOT on that list, and an expression-bodied
// lambda carries no annotation, so the literal `[x, x + 10]` had no target at
// all (the selector's own element type U is inferred FROM the body — circular).
//
// Before the fix the literal stayed untyped, codegen's lowerExpr returned null
// for the CollectionLiteralExpr body, and the lambda fell through to a
// null/zero return — the SelectMany trampoline then wrapped that null as an
// EMPTY inner sequence: `ys` was silently empty (a MISCOMPILE, no diagnostic).
//
// This is the same §15.4 defect that #1174 fixed for a collection literal passed
// DIRECTLY to an `IEnumerable<T>` parameter (`"x".Trim([c])`, `string.Join(...)`,
// `M(xs: IEnumerable<char>)`): a literal whose only target is a collection
// interface must be REJECTED, not materialized into a fabricated concrete type
// (§15.4 gives it no standalone type). The fix diagnoses the lambda-body case
// here too. The idiom for producing a per-element sub-sequence is a block body
// binding an explicitly-typed local — probe p21_selectmany_named, which already
// works:
//
//     xs.SelectMany((x) => do
//       var inner: Array<int> := [x, x + 10];   // concrete §15.4 target
//       return inner;
//     end)
//
// This test pins the diagnostic (so the silent empty-sequence miscompile cannot
// return) and pins that the sanctioned idiom + non-literal selectors still
// flatten correctly (the reject is scoped to the untargetable literal body).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// AOT-compile+run; return the `main` exit code (or -999 if it did not run:
// compile/link reject or program crash/timeout).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// True iff the front end REJECTED `src` at compile time (a diagnostic fired and
// the program did not run). A spurious IR-verification failure would also set
// hasErrors, so `!ran` alone is weaker; we require the explicit diagnostic.
bool rejects(const std::string& src) {
    auto r = runAot(src);
    return r.diagnostics.hasErrors() && !r.ran;
}

// True iff at least one reported diagnostic message contains `needle` (pins the
// specific §15.4 diagnostic rather than any error).
bool diagContains(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — the literal-bodied selector (the #1693 silent miscompile).
// ---------------------------------------------------------------------------

// §15.4 / stdlib §5 (#1693): the exact p9 repro — `SelectMany((x) => [x, x+10])`.
// Load-bearing: before the fix this COMPILED (exit 0) and iterated an empty
// sequence; it must now be rejected, not silently miscompiled to empty.
BEZEL_TEST(selectmany_literal_selector_1693, selectmany_literal_body_rejected_15_4) {
    BEZEL_EXPECT_EQ(rejects(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2]; "
        "  var n := 0; "
        "  for var y in xs.SelectMany(lambda(x) => [x, x + 10]) do n := n + 1; end; "
        "  return n; "
        "end Main; end C;"), true);
}

// §15.4 (#1693): the rejection is the §15.4 collection-literal-target diagnostic
// (not an incidental downstream error). Pin the section citation and the
// literal-target wording so the message can't drift into a generic error.
BEZEL_TEST(selectmany_literal_selector_1693, selectmany_literal_body_cites_15_4) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2]; "
        "  var n := 0; "
        "  for var y in xs.SelectMany(lambda(x) => [x, x + 10]) do n := n + 1; end; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(diagContains(src, "\xC2\xA7""15.4"));
    BEZEL_EXPECT_TRUE(diagContains(src, "collection literal"));
}

// §15.4 (#1693): the same rule applies to any LINQ selector whose lambda body is
// a bare collection literal — `Select((x) => [x, x + 10])` (whose result element
// type is itself the untargetable literal) is rejected too, not miscompiled.
BEZEL_TEST(selectmany_literal_selector_1693, select_literal_body_rejected_15_4) {
    BEZEL_EXPECT_EQ(rejects(
        "module M; class C public static method Main(): int "
        "  var xs: Array<int> := [1, 2]; "
        "  var n := 0; "
        "  for var row in xs.Select(lambda(x) => [x, x + 10]) do n := n + 1; end; "
        "  return n; "
        "end Main; end C;"), true);
}

// ---------------------------------------------------------------------------
// NO REGRESSION — the sanctioned idiom and non-literal selectors still flatten.
// ---------------------------------------------------------------------------

// §15.4 / stdlib §5 (#1693, probe p21): the sanctioned form — a block body
// binding an explicitly-typed `Array<int>` local (a concrete §15.4 target) and
// returning it — flattens correctly. xs=[1,2], selector x -> [x, x+10] gives
// [1,11,2,12]: sum 26, count 4. Load-bearing: proves the reject is scoped to the
// untargetable literal body and did not disable literal-in-lambda entirely.
BEZEL_TEST(selectmany_literal_selector_1693, block_body_typed_local_flattens_15_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var xs := new Array<int>(); xs.Add(1); xs.Add(2); "
        "  var s := 0; var n := 0; "
        "  for var y in xs.SelectMany(lambda(x) => do "
        "      var inner: Array<int> := [x, x + 10]; return inner; end) do "
        "    s := s + y; n := n + 1; "
        "  end; "
        "  return int(s = 26 and n = 4); "                    // 1 + 11 + 2 + 12
        "end Main; end C;"), 1);
}

// stdlib §5 (#1693 no-regression): a selector that returns a concrete collection
// via a method call (not a bare literal) still flattens. Same [1,11,2,12]. This
// is the non-literal control: the §15.4 reject fires ONLY on the untargetable
// literal body, never on a selector that yields a real IEnumerable<U>.
BEZEL_TEST(selectmany_literal_selector_1693, method_returning_array_selector_flattens) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Pair(x: int): owned Array<int> "
        "    var r := new Array<int>(); r.Add(x); r.Add(x + 10); return r; "
        "  end Pair; "
        "  public static method Main(): int "
        "    using var xs := new Array<int>(); xs.Add(1); xs.Add(2); "
        "    var s := 0; var n := 0; "
        "    for var y in xs.SelectMany(lambda(x) => C.Pair(x)) do s := s + y; n := n + 1; end; "
        "    return int(s = 26 and n = 4); "                   // 1 + 11 + 2 + 12
        "  end Main; end C;"), 1);
}

// stdlib §5 (#1693 no-regression): a selector whose body is a LAZY LINQ chain
// (#758) — an ordinary non-literal expression — still flattens. This guards the
// common `(n) => src.Where(...)` shape against an over-broad reject.
BEZEL_TEST(selectmany_literal_selector_1693, lazy_inner_selector_flattens) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var arr := new Array<int>(); arr.Add(1); arr.Add(2); arr.Add(3); "
        "  var s := 0; "
        "  for var x in arr.SelectMany(lambda(n) => arr.Where(lambda(m) => m <= n)) do s := s + x; end; "
        "  return s; "                                         // 1 + (1+2) + (1+2+3) = 10
        "end Main; end C;"), 10);
}
