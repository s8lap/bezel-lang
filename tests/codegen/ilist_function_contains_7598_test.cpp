// Codegen + sema regression — issue #7598: `IList<(int) -> int>.Contains(f)`
// was rejected as "requires `T: hashable`", but `IList<T>.Contains` IS
// `ICollection<T>.Contains` — a stdlib §4 EQUALITY SEARCH, not §5's
// hashable-block extension operator.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// stdlib §4 (*Equality search at an element type with no equality*,
// docs/Bezel_Standard_Library.md, v0.355): "The members that compare an
// argument with stored elements by `Equals` are the equality search:
// `Contains` and `Remove(item)` of `ICollection<T>` and of its
// implementations here at an unconstrained element type (`Array<T>`,
// `LinkedList<T>`), `Contains` of `Stack<T>` and `Queue<T>`, `Array<T>.IndexOf`
// and `LastIndexOf`, `LinkedList<T>.Find`, and the span family's `IndexOf`,
// `Contains` and `SequenceEqual`. Their signatures are unconstrained, and they
// are conditionally-admitted members (language spec §14.2): a call is admitted
// only where the element type, nullability stripped, has a member for the
// comparison language spec §9.2 calls the first kind, `Equals` with an
// argument of that type."
//
// stdlib §4.1 (v0.355) declares `method Contains(item: T): bool;` on
// `ICollection<T>`, and `public interface IList<T>: IReadOnlyList<T>,
// ICollection<T>` — so `IList<T>.Contains` IS `ICollection<T>.Contains`.
//
// spec §26.5 rule 1 (v0.355): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` … instance resolution is
// committed and extension blocks are **never consulted** for that name on
// that receiver."  `IList<T>` DECLARES `Contains` (inherited), so §5's
// `extension IEnumerable<T> where T: hashable` block — whose `Contains` is the
// operator the rejection named — is never consulted for it.
//
// spec §27.7.1 (v0.355): "A **lambda** (closure) value equals only itself —
// reference equality on the closure instance. Two separately-constructed
// lambdas with identical source text are *not* equal, even if they capture the
// same variables."  So a function element type HAS the §9.2 first-kind
// `Equals` §4 asks for.
//
// WHAT WAS WRONG (MEASURED on r30-c1, the base of this lane).
//
//   operator `Contains` requires `T: hashable` (§14.2; stdlib §5, proposal
//   0116 D9): it is a function type, which is not **hashable** (§3.6 clause
//   (e); §27.7.1: a lambda equals only itself) — keep callbacks in an
//   `Array<...>` and use its `Contains`
//
// — for `IList<(int) -> int>.Contains(f)`, while the SAME §4 member at
// `Array<(int) -> int>` and at `ICollection<(int) -> int>` was admitted.
//
// THE ROWS.
//
//  (A) The over-rejection is gone and the search WORKS: `IList<fn>.Contains(f)`
//      is admitted, answers TRUE for the stored element, and — per §27.7.1 —
//      FALSE for a separately-constructed lambda with identical source text.
//      The `Array<fn>` twin is the positive control: same verdicts.
//  (B) The `IReadOnlyList<fn>` twin stays REJECTED: `IReadOnlyList<T>`
//      declares NO `Contains` (stdlib §4.1), so its `Contains` IS §5's
//      extension operator and §5's `where T: hashable` governs it.  The
//      over-rejection fix must NOT widen the admission.
//  (C) An element type with NO first-kind `Equals` (a record with a function
//      field — neither auto-hashable nor declaring an `Equals` of its own,
//      §9.2) is still REJECTED at an `IList` receiver — and the diagnostic
//      now names stdlib §4 (*Equality search*) and the §9.2 first-kind
//      `Equals`, NOT §5's `where T: hashable` block: a "requires `T:
//      hashable`" diagnostic at a §4 member would name a condition the
//      ADMITTED programs of row (A) fail (a function type is not hashable, yet
//      the §4 search admits it).
//
// ORACLE NOTES.  `runAot` drives the real AOT path (fork-isolated
// `Driver::compileFile` + clang link + native exec).  The rows read the
// program's DATA (the two Contains verdicts), not a constant.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// ---------------------------------------------------------------------------
// (A) The equality search at an `IList` receiver.
//
// `f` is the element the array holds; `g` is a SEPARATELY-CONSTRUCTED lambda
// with identical source text — per §27.7.1 the two are NOT equal, so the
// search answers TRUE for `f` and FALSE for `g`.
// ---------------------------------------------------------------------------
const char* kIListEqualitySearch =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var f: (int) -> int := lambda(x: int) => x + 1; "
    "    var g: (int) -> int := lambda(x: int) => x + 1; "
    "    var arr: Array<(int) -> int> := new Array<(int) -> int>(); "
    "    arr.Add(f); "
    "    var lst: IList<(int) -> int> := arr; "
    "    var arrC: Array<(int) -> int> := arr; "
    "    Console.WriteLine(lst.Contains(f)); "     // True  — the stored element
    "    Console.WriteLine(lst.Contains(g)); "     // False — a different lambda
    "    Console.WriteLine(arrC.Contains(f)); "    // True  — the Array control
    "    Console.WriteLine(arrC.Contains(g)); "    // False — the Array control
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// (B) The negative control: `IReadOnlyList<T>` declares no `Contains`, so
// its `Contains` is §5's hashable-block extension operator and a function
// element type is refused there.
// ---------------------------------------------------------------------------
const char* kIReadOnlyListStillRejected =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var f: (int) -> int := lambda(x: int) => x + 1; "
    "    var arr: Array<(int) -> int> := new Array<(int) -> int>(); "
    "    arr.Add(f); "
    "    var rol: IReadOnlyList<(int) -> int> := arr; "
    "    var b := rol.Contains(f); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// (C) An element type with NO §9.2 first-kind `Equals`: a record with a
// function-typed field is neither auto-hashable nor declares an `Equals` of
// its own (stdlib §4: "It has none exactly where … a record or choice that
// is neither auto-hashable nor declares an `Equals` of its own").
// ---------------------------------------------------------------------------
const char* kNonEqualityBearingElement =
    "module M; "
    "public record RX(fn: (int) -> int) end RX; "
    "class C "
    "  public static method Main(): int "
    "    var f: (int) -> int := lambda(x: int) => x + 1; "
    "    var arr: Array<RX> := new Array<RX>(); "
    "    arr.Add(new RX(f)); "
    "    var lst: IList<RX> := arr; "
    "    var b := lst.Contains(new RX(f)); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------------
// (A) the over-rejection is gone and the search works.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_ilist_function_contains_7598,
           ilist_function_contains_is_an_equality_search_stdlib_4_v0355) {
    JitResult r = runAot(kIListEqualitySearch);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nTrue\nFalse\n");
}

// ---------------------------------------------------------------------------
// (B) the negative control: `IReadOnlyList<fn>.Contains` stays refused.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_ilist_function_contains_7598,
           ireadonlylist_function_contains_still_refused_stdlib_5_v0355) {
    JitResult r = runAot(kIReadOnlyListStillRejected);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cited = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("requires `T: hashable`") != std::string::npos)
            cited = true;
    BEZEL_EXPECT_TRUE(cited);
}

// ---------------------------------------------------------------------------
// (C) an element type with no first-kind `Equals` is still refused, and the
// diagnostic names stdlib §4's equality search, not §5's hashable block.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_ilist_function_contains_7598,
           ilist_non_equality_bearing_element_still_refused_stdlib_4_v0355) {
    JitResult r = runAot(kNonEqualityBearingElement);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cited = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("stdlib §4 (*Equality search*)") != std::string::npos &&
            d.message.find("first-kind `Equals`") != std::string::npos)
            cited = true;
    BEZEL_EXPECT_TRUE(cited);
}
