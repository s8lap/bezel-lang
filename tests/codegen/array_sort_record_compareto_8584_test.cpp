// Regression tests for issue #8584 — `Array<T>.Sort()` / `Array<T>.Sorted()`
// over a user RECORD element that declares `CompareTo`.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §4.4 declares, on `Array<T>`:
//         public method Sort() where T: IComparable<T>;                  // in place
//         public method Sorted(): owned Array<T> where T: IComparable<T>; // fresh copy
//     and states of the parameterless natural-order form: "for a concrete
//     element type the natural-order comparer is that type's own `CompareTo`
//     (for example `int.CompareTo`)".
//   * that member's meaning is given by stdlib §2.8 (`IComparable<T>`):
//     "`a.CompareTo(b)` returns a negative number if `a < b`, zero if equal, a positive number if
//     `a > b`" — and the code-generation reference §5 calls the parameterless
//     form the "natural order" of the element type, i.e. the order ITS OWN
//     `CompareTo` defines (a type whose `CompareTo` orders descending is
//     naturally ordered descending).
//   * spec §14.2 *Interface conformance* rule 2 is what admits the record at
//     all: "T is a record that lists I (or any interface extending I) in its
//     inheritance clause and declares matching members" — so a
//     `public record Pair … : IComparable<Pair>` that declares
//     `CompareTo(other: Pair): int` conforms, and §4.4 then requires the
//     ORDER to be that member's.
//
// THE DEFECT. The comparator selection in `Array<T>.Sort()` / `Array<T>.Sorted()`
// had no arm for a RECORD: the class arm requires `TypeDeclKind::Class`, the
// interface arm requires an interface, the `string` arm needs a `ptrTy()`
// element — and a record is a VALUE type stored INLINE in the array slot, so
// none of them matched and the order fell through to the `bz$intcmp`
// fallback, which loads an i32 out of the element SLOT. For a record that
// slot IS the record's first `int`-shaped field, so the elements were ranked
// by THAT FIELD and the user's `CompareTo` was never called.
//
// MEASURED on the base (`r30-c1` = c95dca2c0): the issue's own repro below
// printed `7:30 8:10 9:20` — the `tag` order — where §4.4 requires
// `8:10 9:20 7:30` (the `cents` order its `CompareTo` defines).
//
// WHY THE ORACLE IS stdout AND NOT AN EXIT CODE. The subject is the ORDER of
// the elements; a wrong order and the right order both exit 0. Each row
// PRINTS the post-sort sequence.
//
// WHY `runAotChecked` AND NOT `runAot`. A record's `CompareTo(other: Pair)`
// receives `other` BY VALUE (§9.4/CG29) — a COPY of the other element's
// bytes — and a record may hold REFERENCES (the string-field row below), so
// the checked oracle (the generated code compiled with AddressSanitizer) is
// the real net for a wrong ABI here, not a formality.
//
// WHY EVERY ROW LOOPS 200 TIMES. The sort is repeated 200× inside the
// program, so the comparator runs hundreds of times against borrowed
// `other` operands and a leak or a use-after-free in the dispatch is not a
// one-shot accident the sanitizer can miss (CLAUDE.md, *Test and oracle
// rules*: "one-shot leak repros (loop 50–500×)").
//
// THE ONE CONTROL. The class-element row is green on the BASE — the class
// path already dispatched `a.CompareTo(b)` correctly (#1195) — so it is the
// suite's named boundary control: it would go RED under a wrong fix that
// routed a CLASS element through the new record thunk (the two thunks have
// different ABIs: a class's `CompareTo` takes its operand BY REFERENCE, a
// record's BY VALUE). Every other row here is red on the base.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The issue's own record (verbatim from #8584's repro): two `int` fields, and
// a `CompareTo` that orders by the SECOND one — so the order §4.4 requires
// (by `CompareTo`) and the order the pre-fix fallback produced (by the first
// `int` field) DISAGREE.
const char* kPair =
    "module M; "
    "public record Pair(tag: int, cents: int): IComparable<Pair> "
    "  public method CompareTo(other: Pair): int "
    "    if cents < other.cents then return -1; end if; "
    "    if cents > other.cents then return 1; end if; "
    "    return 0; "
    "  end CompareTo; "
    "end Pair; ";

// The issue's own element sequence: `tag` order 9, 7, 8 and `cents` order
// 20, 30, 10 — the two orders disagree.
const char* kPairElems =
    "[new Pair(9, 20), new Pair(7, 30), new Pair(8, 10)]";

// The expected post-sort sequence under §4.4: by `cents` (10, 20, 30).
const char* kPairByCents = "8:10 9:20 7:30 \n";
// The sequence the RECEIVER keeps after `Sorted()` (stdlib §4.4: "the
// past-participle form (`Reversed`, `Sorted`) leaves the receiver unchanged").
const char* kPairUnchanged = "9:20 7:30 8:10 \n";

std::string outChecked(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// Print every element of `v` as "<tag>:<cents> " on ONE line.
std::string printPairElems(const std::string& v) {
    return "for var e in " + v + " do Console.Write($\"{e.tag}:{e.cents} \"); "
           "end; Console.WriteLine();";
}

// The 200× loop, so the comparator is exercised hundreds of times under the
// sanitizer (a one-shot sort can hide a wrong ABI).
const char* kLoop200 = "for var i in 0..<200 do a.Sort(); end for; ";

}  // namespace

// ── THE ISSUE'S OWN REPRO ────────────────────────────────────────────────────
// §4.4: "for a concrete element type the natural-order comparer is that
// type's own `CompareTo`". The record's `CompareTo` orders by `cents`, so the
// elements come out 10, 20, 30 cents. The pre-fix fallback ranked them by
// `tag` and printed `7:30 8:10 9:20`.
BEZEL_TEST(array_sort_record_compareto_8584, record_sort_orders_by_compare_to) {
    BEZEL_EXPECT_EQ(
        outChecked(std::string(kPair) + "public class C "
                   "public static method Main(): int "
                   "var a: Array<Pair> := " +
                   kPairElems + "; " + kLoop200 + printPairElems("a") +
                   "return 0; end Main; end C;"),
        std::string(kPairByCents));
}

// The `Sorted()` TWIN: the copy form of the same stdlib §4.4 member. It must
// impose the SAME order (the two arms share one comparator) and must leave
// the RECEIVER unchanged (stdlib §4.4).
BEZEL_TEST(array_sort_record_compareto_8584,
           record_sorted_copy_form_orders_by_compare_to) {
    BEZEL_EXPECT_EQ(
        outChecked(std::string(kPair) + "public class C "
                   "public static method Main(): int "
                   "var a: Array<Pair> := " +
                   kPairElems + "; " +
                   "for var i in 0..<200 do using var s := a.Sorted(); "
                   "end for; " +
                   "using var s2 := a.Sorted(); " + printPairElems("s2") +
                   printPairElems("a") + "return 0; end Main; end C;"),
        std::string(kPairByCents) + kPairUnchanged);
}

// The STATIC form `Array.Sort(a)` (fix #3345's spelling: the static-call form
// re-binds the receiver to `args[0]`, so it lowers through the same arm). A
// record element must be ranked by its `CompareTo` here too.
//
// The array is built with `new Array<Pair>()` + `Add` (the house idiom of the
// #1195 rows) rather than an array LITERAL: the static-call form SKIPS the
// array-literal temporary's cleanup (the `freeUnboundOwnedBorrowingArrayTemp`
// path is gated on a receiver the static form never produces), so an array
// LITERAL leaks its backing buffer under the sanitizer. That leak is a
// PRE-EXISTING, SEPARATE defect — it reproduces with an `int` element, whose
// comparator path this fix does not touch — and it is filed separately; this
// row uses the leak-free spelling so that the row pins the ORDER and nothing
// else.
BEZEL_TEST(array_sort_record_compareto_8584,
           record_static_array_sort_orders_by_compare_to) {
    BEZEL_EXPECT_EQ(
        outChecked(std::string(kPair) + "public class C "
                   "public static method Main(): int "
                   "using var a := new Array<Pair>(); "
                   "a.Add(new Pair(9, 20)); "
                   "a.Add(new Pair(7, 30)); "
                   "a.Add(new Pair(8, 10)); "
                   "for var i in 0..<200 do Array.Sort(a); end for; " +
                   printPairElems("a") +
                   "return 0; end Main; end C;"),
        std::string(kPairByCents));
}

// A GENERIC record: §14.1 — the `CompareTo` that orders `R<int>` is the one
// MONOMORPHIZED at `R<int>`, so the order is read at the instantiation, not
// at the erased template. The record's `CompareTo` orders by `n` (its second
// field), the pre-fix fallback ranked by `k` (its first `int` field).
BEZEL_TEST(array_sort_record_compareto_8584,
           generic_record_sort_orders_by_compare_to) {
    const char* kGeneric =
        "module M; "
        "public record R<T>(k: int, n: int, payload: T): IComparable<R<T>> "
        "  public method CompareTo(other: R<T>): int "
        "    if n < other.n then return -1; end if; "
        "    if n > other.n then return 1; end if; "
        "    return 0; "
        "  end CompareTo; "
        "end R; ";
    BEZEL_EXPECT_EQ(
        outChecked(std::string(kGeneric) + "public class C "
                   "public static method Main(): int "
                   "var a: Array<R<int>> := [new R<int>(1, 30, 7), "
                   "new R<int>(2, 10, 8), new R<int>(3, 20, 9)]; " +
                   kLoop200 +
                   "for var e in a do Console.Write($\"{e.k}:{e.n} \"); end; "
                   "Console.WriteLine(); "
                   "return 0; end Main; end C;"),
        std::string("2:10 3:20 1:30 \n"));
}

// A record whose `CompareTo` orders DESCENDING. §4.4 fixes the order
// through the element's own `CompareTo` (the code-generation reference calls
// it the element type's "natural order"), so a type whose `CompareTo` ranks
// the larger value first is ranked larger-first. The pre-fix fallback ranked
// by the first `int` field instead, which for this data is the opposite
// order.
BEZEL_TEST(array_sort_record_compareto_8584,
           record_descending_compare_to_orders_descending) {
    const char* kDesc =
        "module M; "
        "public record D(a: int, b: int): IComparable<D> "
        "  public method CompareTo(other: D): int "
        "    if b < other.b then return 1; end if; "
        "    if b > other.b then return -1; end if; "
        "    return 0; "
        "  end CompareTo; "
        "end D; ";
    BEZEL_EXPECT_EQ(
        outChecked(std::string(kDesc) + "public class C "
                   "public static method Main(): int "
                   "var a: Array<D> := [new D(1, 1), new D(2, 3), new D(3, 2)]; " +
                   kLoop200 +
                   "for var e in a do Console.Write($\"{e.a}:{e.b} \"); end; "
                   "Console.WriteLine(); "
                   "return 0; end Main; end C;"),
        std::string("2:3 3:2 1:1 \n"));
}

// A record whose FIRST field is a `string`. The pre-fix fallback loaded an
// i32 out of the element SLOT — here the low half of the string REFERENCE —
// so the order was decided by pointer values rather than by `CompareTo`. The
// elements are created in DESCENDING `n` order, so the pointer order (which
// follows the creation order) is the exact reverse of the `CompareTo` order:
// the row cannot pass by accident on the base.
//
// The row is also the one that exercises a record that HOLDS A REFERENCE:
// `CompareTo(other: S)` receives `other` BY VALUE (§9.4/CG29), a copy of the
// other element's bytes, so the copy's string reference must not be released
// twice — the sanitizer is the net for that, over 200 iterations.
BEZEL_TEST(array_sort_record_compareto_8584,
           record_string_field_first_orders_by_compare_to) {
    const char* kStr =
        "module M; "
        "public record S(name: string, n: int): IComparable<S> "
        "  public method CompareTo(other: S): int "
        "    if n < other.n then return -1; end if; "
        "    if n > other.n then return 1; end if; "
        "    return 0; "
        "  end CompareTo; "
        "end S; ";
    BEZEL_EXPECT_EQ(
        outChecked(std::string(kStr) + "public class C "
                   "public static method Main(): int "
                   "var a: Array<S> := [new S(\"six\", 6), new S(\"five\", 5), "
                   "new S(\"four\", 4), new S(\"three\", 3), new S(\"two\", 2), "
                   "new S(\"one\", 1)]; " +
                   kLoop200 +
                   "for var e in a do Console.Write($\"{e.n} \"); end; "
                   "Console.WriteLine(); "
                   "return 0; end Main; end C;"),
        std::string("1 2 3 4 5 6 \n"));
}

// ── THE CONTROL (green on the BASE as well as on the tip) ───────────────────
// A CLASS element with a `CompareTo`: the class path already dispatched
// `a.CompareTo(b)` correctly (#1195), so this row is green on both arms. It
// is the suite's named boundary control — it goes RED under a wrong fix that
// routed a class element through the record thunk (a class's `CompareTo`
// takes its operand BY REFERENCE; a record's takes it BY VALUE — the two
// ABIs are not interchangeable).
//
// The class instances are `using var` locals so they stay live through the
// sort and the reads (the same discipline the #1195 rows use).
BEZEL_TEST(array_sort_record_compareto_8584,
           class_element_control_orders_by_compare_to) {
    const char* kBox =
        "module M; "
        "public class Box(v: int): IComparable<Box> "
        "  public readonly property V: int := v; "
        "  public method CompareTo(other: Box): int "
        "    if v < other.v then return -1; end if; "
        "    if v > other.v then return 1; end if; "
        "    return 0; "
        "  end CompareTo; "
        "end Box; ";
    BEZEL_EXPECT_EQ(
        outChecked(std::string(kBox) + "public class C "
                   "public static method Main(): int "
                   "using var b0 := new Box(30); "
                   "using var b1 := new Box(10); "
                   "using var b2 := new Box(20); "
                   "var a: Array<Box> := new Array<Box>(); "
                   "a.Add(b0); a.Add(b1); a.Add(b2); " + kLoop200 +
                   "for var e in a do Console.Write($\"{e.V} \"); end; "
                   "Console.WriteLine(); "
                   "return 0; end Main; end C;"),
        std::string("10 20 30 \n"));
}
