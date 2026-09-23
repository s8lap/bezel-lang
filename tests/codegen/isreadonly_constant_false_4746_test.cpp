// Codegen + sema — `IsReadOnly` on the builtin collections (#4746).
//
// stdlib §4.1 (v0.307) declares the member and fixes its value in one
// sentence:
//
//   public interface ICollection<T>: IReadOnlyCollection<T>
//     readonly property IsReadOnly: bool;
//     …
//
//   "`IsReadOnly` is `false` for every standard collection implementing
//    `ICollection<T>` (`Array<T>`, `LinkedList<T>`, `Set<T>`,
//    `Dictionary<K, V>`; `Stack<T>`/`Queue<T>` conform only to
//    `IReadOnlyCollection<T>` (#1354) and have no `IsReadOnly` member). The
//    standard library currently ships no read-only `ICollection<T>`
//    implementation …"
//
// §4.2 declares the same member on `IOwningCollection<T>`.
//
// Pre-fix the member had TWO distinct defects, split along the family axis:
//
//   * `Array<T>`/`OwningArray<T>` — the FIRST type §4.1's sentence names —
//     were REJECTED outright by the §4.4/§4.5 catch-all in
//     `ExprTypecheck_Calls.cpp`:
//       "no member `IsReadOnly` on `Array<T>` (stdlib §4.4/§4.5) -- it is not
//        part of this collection's documented property surface …"
//   * `Set`/`OwningSet`/`LinkedList`/`OwningLinkedList`/`Dictionary` reached
//     `isInheritedIsReadOnly` (#2538) and were EXEMPTED from that reject —
//     but nothing typed the read and no codegen arm lowered it, so every
//     `Console.WriteLine(x.IsReadOnly)` was the #3079 silent no-op:
//       "warning: internal: the argument of this `Console.WriteLine` failed
//        to lower to a value, so the whole statement emits nothing — not even
//        the line terminator (stdlib §2.1 / §21.9) — #3079"
//     Program output was NOTHING AT ALL, exit 0. That is why every positive
//     row below asserts the program's STDOUT and not merely that it compiled:
//     a compile-only oracle cannot tell "works" from "emits nothing".
//
// Row 1 is rule-corpus case `l4_1_0003`'s exact program (stdlib §4.1, `run`
// tier, expected stdout `False`).
//
// The negative rows are the boundary: §4.1's own sentence excludes
// `Stack<T>`/`Queue<T>` (#1354), §4.9 gives `OwningDictionary<K, V>` an
// `IDisposable`-only conformance, and §4.17/§4.18 give `SortedDictionary` /
// `SortedSet` `IReadOnlyCollection<…>` only. All four must keep rejecting —
// this fix is an accept flip, and these rows are what stops it widening past
// the families the stdlib actually gives the member to.

#include "support/aot_support.hpp"   // runAot / runAotOut — the AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ── Positive rows — stdlib §4.1/§4.2: `false` on every family that conforms
//    to `ICollection<T>` or `IOwningCollection<T>`. ─────────────────────────

// Rule-corpus case l4_1_0003, verbatim. Pre-fix: rejected at the member
// access, so the program did not compile at all.
BEZEL_TEST(isreadonly_constant_false_4746, array_isreadonly_prints_false) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var arr := new Array<int>(); "
        "  Console.WriteLine(arr.IsReadOnly); "
        "end Main; end C;"), "False");
}

// stdlib §4.5: `OwningArray<T>: IOwningList<T>` → `IOwningCollection<T>`
// (§4.2), which declares the member. Pre-fix this hit the same §4.4/§4.5
// reject as the borrowing form.
BEZEL_TEST(isreadonly_constant_false_4746, owningarray_isreadonly_prints_false) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main() "
        "  using var a := new OwningArray<E>(); "
        "  Console.WriteLine(a.IsReadOnly); "
        "end Main; end C;"), "False");
}

// The issue's half-2 repro, verbatim: three reads that compiled clean and
// printed NOTHING. Three lines of `False` is the whole assertion — an empty
// stdout is exactly the pre-fix behaviour.
BEZEL_TEST(isreadonly_constant_false_4746,
           set_linkedlist_dictionary_isreadonly_all_print_false) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var s := new Set<int>(); "
        "  Console.WriteLine(s.IsReadOnly); "
        "  using var l := new LinkedList<int>(); "
        "  Console.WriteLine(l.IsReadOnly); "
        "  using var d := new Dictionary<int, int>(); "
        "  Console.WriteLine(d.IsReadOnly); "
        "end Main; end C;"), "False\nFalse\nFalse");
}

// stdlib §4.10 / §4.7: the owning twins of the two families above.
BEZEL_TEST(isreadonly_constant_false_4746,
           owningset_and_owninglinkedlist_isreadonly_print_false) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main() "
        "  using var s := new OwningSet<E>(); "
        "  Console.WriteLine(s.IsReadOnly); "
        "  using var l := new OwningLinkedList<E>(); "
        "  Console.WriteLine(l.IsReadOnly); "
        "end Main; end C;"), "False\nFalse");
}

// The #3079 degrade DROPS THE WHOLE STATEMENT — "not even the line
// terminator" — so a program that prints a marker either side of the read
// distinguishes "the statement ran and printed False" from "the statement was
// silently deleted". Pre-fix this program compiled at exit 0 and printed
// exactly "before\nafter" (measured on the reverted compiler).
//
// An earlier draft of this row asserted the ABSENCE of the #3079 warning text
// in `runAot(...).diagnostics`. That oracle is VACUOUS and was measured to be:
// runAot synthesizes a diagnostic only on a compile/link reject or a crash, so
// a program that compiles successfully carries no diagnostics at all and the
// row passed on BOTH sides of the fix. Do not reinstate it — the observable
// for a dropped statement is the program's stdout, not the diagnostic list.
BEZEL_TEST(isreadonly_constant_false_4746,
           the_writeline_statement_is_not_silently_dropped) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  Console.WriteLine(\"before\"); "
        "  using var s := new Set<int>(); "
        "  Console.WriteLine(s.IsReadOnly); "
        "  Console.WriteLine(\"after\"); "
        "end Main; end C;"), "before\nFalse\nafter");
}

// The read is a `bool` VALUE, not merely something `Console.WriteLine` can
// format: it binds to a local and drives a condition (spec §20). A lowering
// that produced no value could not reach either position.
BEZEL_TEST(isreadonly_constant_false_4746,
           isreadonly_binds_to_a_bool_local_and_drives_a_condition) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  using var arr := new Array<int>(); "
        "  var ro := arr.IsReadOnly; "
        "  Console.WriteLine((if ro then \"read-only\" else \"mutable\")); "
        "end Main; end C;"), "mutable");
}

// ── Negative rows — the accept flip must not widen past §4.1/§4.2's own
//    conformance list. Each of these families is named in the stdlib as NOT
//    carrying the member. ───────────────────────────────────────────────────

// stdlib §4.9: `OwningDictionary<K, V>: IDisposable` — it implements no
// interface that declares `IsReadOnly`. (Also pinned by
// dictsetll_bareref_nomember_2536.owningdictionary_isreadonly_still_rejected;
// repeated here because THIS fix is the one that could widen it.)
BEZEL_TEST(isreadonly_constant_false_4746,
           owningdictionary_isreadonly_still_rejected) {
    BEZEL_EXPECT_TRUE(hasError(
        "module M; "
        "class V public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; end V; "
        "class C public static method Main() "
        "  using var d := new OwningDictionary<string, V>(); "
        "  Console.WriteLine(d.IsReadOnly); "
        "end Main; end C;",
        "no member `IsReadOnly` on `OwningDictionary`"));
}

// stdlib §4.1's own sentence + §4.11: `Stack<T>`/`Queue<T>` conform only to
// `IReadOnlyCollection<T>` (#1354) and "have no `IsReadOnly` member".
BEZEL_TEST(isreadonly_constant_false_4746, stack_isreadonly_still_rejected) {
    BEZEL_EXPECT_TRUE(hasError(
        "module M; "
        "class C public static method Main() "
        "  using var s := new Stack<int>(); "
        "  Console.WriteLine(s.IsReadOnly); "
        "end Main; end C;",
        "no member `IsReadOnly` on `Stack<T>`"));
}

BEZEL_TEST(isreadonly_constant_false_4746, queue_isreadonly_still_rejected) {
    BEZEL_EXPECT_TRUE(hasError(
        "module M; "
        "class C public static method Main() "
        "  using var q := new Queue<int>(); "
        "  Console.WriteLine(q.IsReadOnly); "
        "end Main; end C;",
        "no member `IsReadOnly` on `Queue<T>`"));
}

// stdlib §4.17 / §4.18: both conform to `IReadOnlyCollection<…>` only
// (#1354), so neither inherits the member.
BEZEL_TEST(isreadonly_constant_false_4746,
           sorteddictionary_isreadonly_still_rejected) {
    BEZEL_EXPECT_TRUE(hasError(
        "module M; "
        "class C public static method Main() "
        "  using var d := new SortedDictionary<int, int>(); "
        "  Console.WriteLine(d.IsReadOnly); "
        "end Main; end C;",
        "no member `IsReadOnly` on `SortedDictionary`"));
}

BEZEL_TEST(isreadonly_constant_false_4746,
           sortedset_isreadonly_still_rejected) {
    BEZEL_EXPECT_TRUE(hasError(
        "module M; "
        "class C public static method Main() "
        "  using var s := new SortedSet<int>(); "
        "  Console.WriteLine(s.IsReadOnly); "
        "end Main; end C;",
        "no member `IsReadOnly` on `SortedSet`"));
}
