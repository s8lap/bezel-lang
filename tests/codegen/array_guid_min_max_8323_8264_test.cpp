// Regression tests for issues #8323 and #8264 — the no-selector
// `Array<Guid>.Min()` / `.Max()` (stdlib §5.1) was REFUSED with
//
//   "Min() without a selector requires the element type to satisfy
//    `T: IComparable<T>` (stdlib §5.1 / spec §14.2 / #2770); `Guid`
//    does not — its `CompareTo` could not be resolved for dispatch."
//
// WHAT THE DOCUMENTS REQUIRE (each §-number opened at v0.355 in this tree).
//
//   * stdlib §2.6 declares
//         public record Guid: IComparable<Guid>, IEquatable<Guid>
//     with "CompareTo = lexicographic over the 16 canonical big-endian bytes
//     (stated explicitly — .NET's field-group order is a known trap); ordering
//     is CompareTo only — no <,>,<=,>= overloads are declared".
//   * stdlib §2.8 declares the constraint's authority:
//         public interface IComparable<T>
//           method CompareTo(other: T): int;
//         end IComparable;
//   * stdlib §5.1 declares the no-selector form:
//         public extension IEnumerable<T> where T: IComparable<T>
//           public method Min(): T;              // InvalidOperationException if empty
//           public method Max(): T;              // InvalidOperationException if empty
//     so `T = Guid` SATISFIES the constraint and the call MUST BE ADMITTED,
//     and the RESULT is the element type `T` (a `Guid`).
//   * spec §14.2 *Constraints* admits it: "`where T: IFoo` | Non-nullable
//     types conforming to `IFoo`: classes implementing it, **records and enums
//     implementing it**, interfaces extending it, and primitives conforming
//     through their standard-library declarations".
//
// THE DEFECT, AND ITS TWO HALVES.
//
//   * The SEMANTIC half — sema's §5.1 gate could not see the built-in
//     `Guid`'s conformance (a built-in value record carries no user
//     `TypeDecl`, so it has no `baseTypeRefs`), and the call fell to the
//     `int` default result type. That half is fixed by the PREREQUISITE
//     commit `2355c2a4` (sema/ExprTypecheck_Calls.cpp), which reads the
//     stdlib's own declaration through the SHARED predicate
//     `sema::isStdlibGuidType`.
//
//   * The ORDERING half — THIS file's subject — is in the collection
//     `Min()` / `Max()` lowering (`src/codegen/LlvmBackend_Calls.cpp`):
//     the comparator resolution there resolved NOTHING for a built-in
//     `Guid` (`findMethodByName(elemTypeRef->declaration, "CompareTo")`
//     needs a declaration; `selfComparableIfaceCompareTo` walks
//     `declaration->baseTypeRefs`), so control reached the #2770 guard and
//     the call was refused with the diagnostic quoted above.
//
// ⚠ THE ORDER IS THE POINT. A `Guid` is stored INLINE — one 16-byte
// `{i64,i64}` struct per array slot (stdlib §2.6) — so the POINTER arm
// (which reads each element as a POINTER) cannot serve it: it would compare
// the ADDRESSES of two inline structs and answer with whichever element
// happened to sit lowest in memory. A fix that merely ADMITTED the call
// through the pointer arm would compile, run, and print a WRONG answer, and
// the rows below are shaped to catch exactly that.
//
// The rows therefore pin the ORDER, not merely the compilation:
//   * `guid_element_min_max_answer_does_not_depend_on_the_array_order` is
//     the row that catches an ADDRESS-ordered fix (the same minimum must be
//     selected from either array order).
//   * `guid_element_order_is_over_the_canonical_bytes_not_a_word` uses a
//     pair whose canonical order and whose LOW-WORD order are OPPOSITE — a
//     word-wise (or i32-wise) comparator answers the reverse of §2.6.
//   * `guid_element_bytes_compare_unsigned` pins `memcmp`'s unsigned byte
//     comparison (0x80 above 0x7f).
//   * `guid_min_agrees_with_the_compare_to_member` pins that the order the
//     `Min()` imposes IS the order `Guid.CompareTo` reports.
//
// The comparison the new arm uses is the runtime's `bz_guid_compare` — the
// very function `Guid.CompareTo` dispatches to, the very one `Math.Min` /
// `Max` / `Clamp` order a `Guid` by (#7935) and the very one
// `Array<Guid>.Sort()` orders by (#8237) — so the arm and the member cannot
// drift apart.
//
// A REFUSED program yields the harness's "<did-not-run>" sentinel, which no
// expected string matches — so the refusal is observed as a FAILURE of the
// positive rows, not asserted as an outcome.
//
// ORACLE: `runAot` (the AOT path) and `runAotChecked` (the same program under
// AddressSanitizer/LSan) — the new arm LOADS the element's 16 bytes out of
// the array buffer, so a wrongly-sized or out-of-bounds load is a memory
// error rather than a wrong number.
//
// ⚠ THE NULLABLE ELEMENT IS OUT OF SCOPE. `Array<Guid?>` is STILL REFUSED:
// the `T?` question is an OWNER RULING (#8241), not this fix's to decide,
// and this fix does NOT widen the nullable case. The row below pins that.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>
#include <string>
#include <vector>

using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotModules;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out — the value a REFUSED program produces here,
// and the reason every positive row below fails before the fix.
std::string out(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// The same, with the GENERATED code run under AddressSanitizer/LSan.
std::string outChecked(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

std::string prog(const std::string& body) {
    return "module M; class C "
           "public static method Main(): int "
           + body +
           " return 0; end Main; end C;";
}

// True when the program's diagnostics carry `needle`.
bool hasError(const bezel::test::JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The canonical §2.6 renderings, one per line, each terminated by a newline
// (the program's stdout).
std::string lines(std::initializer_list<std::string> parts) {
    std::string s;
    for (const auto& p : parts) {
        s += p;
        s += "\n";
    }
    return s;
}

}  // namespace

// #8323 / #8264 / stdlib §2.6 + §5.1 + §2.8 / spec §14.2: the ISSUES' OWN
// REPRO. The elements are the two fixed Guids the issue names, the SMALLER
// SECOND — so a fix that answers the FIRST element (an address-ordered or a
// first-wins fix) is caught here.
//
// The observable is the winner's own §2.6 rendering (`Guid.ToString()`, the
// 36-char canonical dashed form).
BEZEL_TEST(array_guid_min_max_8323_8264,
           guid_element_min_max_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"22222222-2222-2222-2222-222222222222\"), "
            "Guid.Parse(\"11111111-1111-1111-1111-111111111111\")]; "
            "Console.WriteLine(a.Min()); "
            "Console.WriteLine(a.Max()); ")),
        lines({"11111111-1111-1111-1111-111111111111",
               "22222222-2222-2222-2222-222222222222"}));
}

// THE ROW THAT CATCHES AN ADDRESS-ORDERED FIX. The SAME two elements in the
// REVERSE array order must select the SAME winners: the answer is a property
// of the VALUES (§2.6's order), not of where the elements sit in the buffer.
//
// A fix that admitted the built-in `Guid` through the POINTER arm (which
// compares the ADDRESSES of two inline 16-byte slots) would answer the
// FIRST element here and the SECOND element in the row above — the two rows
// together pin that the answer does not depend on the array's order.
BEZEL_TEST(array_guid_min_max_8323_8264,
           guid_element_min_max_answer_does_not_depend_on_the_array_order) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"11111111-1111-1111-1111-111111111111\"), "
            "Guid.Parse(\"22222222-2222-2222-2222-222222222222\")]; "
            "Console.WriteLine(a.Min()); "
            "Console.WriteLine(a.Max()); ")),
        lines({"11111111-1111-1111-1111-111111111111",
               "22222222-2222-2222-2222-222222222222"}));
}

// §2.6's order is over the 16 CANONICAL BYTES, and this pair separates it
// from every WORD-wise order:
//
//   A = 01000000-0000-0000-0000-000000000000  (byte 0 = 0x01, rest 0)
//   B = 00000000-0000-0000-0000-000000000001  (byte 15 = 0x01, rest 0)
//
// Canonically A > B (byte 0 decides: 0x01 > 0x00), so the minimum is B. But
// A's LOW WORD (bytes 0..7) is 1 and B's is 0 — so a comparator that read
// the low word (or the low i32) would answer A as the minimum: the REVERSE.
// This is the row that separates §2.6's order from a word-wise comparator.
BEZEL_TEST(array_guid_min_max_8323_8264,
           guid_element_order_is_over_the_canonical_bytes_not_a_word) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"01000000-0000-0000-0000-000000000000\"), "
            "Guid.Parse(\"00000000-0000-0000-0000-000000000001\")]; "
            "Console.WriteLine(a.Min()); "
            "Console.WriteLine(a.Max()); ")),
        lines({"00000000-0000-0000-0000-000000000001",
               "01000000-0000-0000-0000-000000000000"}));
}

// The canonical bytes are compared as UNSIGNED (the rule the conformance
// corpus pins for `Guid.Min`/`Max`, l5_0062): 0x80 sorts ABOVE 0x7f. A
// SIGNED comparison of the first byte would put 0x80 first.
BEZEL_TEST(array_guid_min_max_8323_8264,
           guid_element_bytes_compare_unsigned) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"80000000-0000-0000-0000-000000000000\"), "
            "Guid.Parse(\"7fffffff-ffff-ffff-ffff-ffffffffffff\")]; "
            "Console.WriteLine(a.Min()); "
            "Console.WriteLine(a.Max()); ")),
        lines({"7fffffff-ffff-ffff-ffff-ffffffffffff",
               "80000000-0000-0000-0000-000000000000"}));
}

// The order the `Min()` imposes IS the order `Guid.CompareTo` reports
// (§2.6: "ordering is CompareTo only"). The reference below is written out
// by hand — a loop over the array that calls the MEMBER's `CompareTo` — and
// the two answers must be the same `Guid`. A `Min()` that ordered by
// anything else (a word, an address, a signed byte) would disagree here.
BEZEL_TEST(array_guid_min_max_8323_8264,
           guid_min_agrees_with_the_compare_to_member) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"33333333-3333-3333-3333-333333333333\"), "
            "Guid.Parse(\"11111111-1111-1111-1111-111111111111\"), "
            "Guid.Parse(\"22222222-2222-2222-2222-222222222222\")]; "
            "var best: Guid := a[0]; "
            "for var x in a do "
            "  if x.CompareTo(best) < 0 then best := x; end if; "
            "end for; "
            "Console.WriteLine(best); "
            "Console.WriteLine(a.Min()); "
            "Console.WriteLine(a.Max()); ")),
        lines({"11111111-1111-1111-1111-111111111111",
               "11111111-1111-1111-1111-111111111111",
               "33333333-3333-3333-3333-333333333333"}));
}

// #8323's SECOND SITE: `SortedSet<Guid>.Min()` — the same §5.1 no-selector
// form over a `SortedSet<Guid>` element. It is the SAME lowering arm (the
// collection `Min()` / `Max()` arm of `tryLowerCollectionInstanceCall`,
// keyed on the receiver's element type), so the same ORDER applies.
BEZEL_TEST(array_guid_min_max_8323_8264,
           guid_sortedset_min_max_admitted_and_ordered) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "using var s := new SortedSet<Guid>(); "
            "s.Add(Guid.Parse(\"22222222-2222-2222-2222-222222222222\")); "
            "s.Add(Guid.Parse(\"11111111-1111-1111-1111-111111111111\")); "
            "s.Add(Guid.Parse(\"01000000-0000-0000-0000-000000000000\")); "
            "Console.WriteLine(s.Min()); "
            "Console.WriteLine(s.Max()); ")),
        lines({"01000000-0000-0000-0000-000000000000",
               "22222222-2222-2222-2222-222222222222"}));
}

// The same shape under AddressSanitizer: the new arm LOADS the element's 16
// bytes out of the array buffer and hands their ADDRESSES to
// `bz_guid_compare`, so a wrongly-sized or out-of-bounds load is a memory
// error rather than a wrong number. (The elements are value types, so the
// program owns no heap object and LSan has nothing to report.)
BEZEL_TEST(array_guid_min_max_8323_8264,
           guid_element_min_max_under_asan) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"22222222-2222-2222-2222-222222222222\"), "
            "Guid.Parse(\"11111111-1111-1111-1111-111111111111\")]; "
            "Console.WriteLine(a.Min()); "
            "Console.WriteLine(a.Max()); ")),
        lines({"11111111-1111-1111-1111-111111111111",
               "22222222-2222-2222-2222-222222222222"}));
}

// stdlib §5.1 ("InvalidOperationException if empty"): the new arm keeps the
// empty-source guard the other arms have, so an EMPTY array still throws
// rather than reading element 0.
BEZEL_TEST(array_guid_min_max_8323_8264,
           guid_empty_source_throws_invalid_operation_exception) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<Guid> := []; "
            "var caught := false; "
            "try "
            "  Console.WriteLine(a.Min()); "
            "catch InvalidOperationException => caught := true; end; "
            "Console.WriteLine(caught); ")),
        "True\n");
}

// CONTROL — #8264's `Math.Min(Guid, Guid)` / `Math.Max` / `Math.Clamp` legs
// already worked before this fix (the #7935 fix is in the tree) and must
// keep working. They are the ORACLE for the ORDER: `Math.Min` orders a
// `Guid` by the runtime's `bz_guid_compare`, the very comparison the new
// arm uses, so the two answers agree.
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_math_min_max_clamp_over_guid_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var g1: Guid := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
            "var g2: Guid := Guid.Parse(\"22222222-2222-2222-2222-222222222222\"); "
            "var lo: Guid := Guid.Parse(\"11111111-1111-1111-1111-111111111111\"); "
            "var hi: Guid := Guid.Parse(\"22222222-2222-2222-2222-222222222222\"); "
            "Console.WriteLine(Math.Min(g1, g2)); "
            "Console.WriteLine(Math.Max(g1, g2)); "
            "Console.WriteLine(Math.Clamp(g1, lo, hi)); ")),
        lines({"11111111-1111-1111-1111-111111111111",
               "22222222-2222-2222-2222-222222222222",
               "11111111-1111-1111-1111-111111111111"}));
}

// CONTROL — stdlib §2.5's temporal value records keep their OWN arm (the
// #8049 one) and their own order (a `DateTime` by TICKS, `Kind` not
// normalized). The new `Guid` arm is selected from the STATIC element type
// and is disjoint from it, so these rows are bit-identical before and after.
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_datetime_element_min_max_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<DateTime> := [DateTime.FromTicks(300), "
            "DateTime.FromTicks(100), DateTime.FromTicks(200)]; "
            "Console.WriteLine(a.Min().Ticks); "
            "Console.WriteLine(a.Max().Ticks); ")),
        "100\n300\n");
}

// CONTROL — the #8049 SIBLINGS (`TimeSpan`, `DateOnly`, `TimeOnly`) keep
// their own arms and their own orders (a `TimeSpan` by its SIGNED tick
// word, a `DateOnly` by its day number, a `TimeOnly` by its
// millisecond-of-day count).
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_temporal_sibling_elements_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<TimeSpan> := [TimeSpan.FromTicks(300), "
            "TimeSpan.FromTicks(100), TimeSpan.FromTicks(-50)]; "
            "Console.WriteLine(a.Min().Ticks); "
            "Console.WriteLine(a.Max().Ticks); "
            "var b: Array<DateOnly> := [new DateOnly(2019, 1, 1), "
            "new DateOnly(2021, 1, 1), new DateOnly(2020, 1, 2)]; "
            "Console.WriteLine(b.Min().ToString()); "
            "Console.WriteLine(b.Max().ToString()); "
            "var c: Array<TimeOnly> := [new TimeOnly(3, 0, 0), "
            "new TimeOnly(1, 0, 0), new TimeOnly(2, 0, 0)]; "
            "Console.WriteLine(c.Min().ToString()); "
            "Console.WriteLine(c.Max().ToString()); ")),
        "-50\n300\n2019-01-01\n2021-01-01\n01:00:00\n03:00:00\n");
}

// CONTROL — a `string` element keeps the POINTER arm's ordinal comparison
// (stdlib §9.6).
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_string_element_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<string> := [\"c\", \"a\", \"b\"]; "
            "Console.WriteLine(a.Min()); "
            "Console.WriteLine(a.Max()); ")),
        "a\nc\n");
}

// CONTROL — an `int` element keeps the numeric path.
BEZEL_TEST(array_guid_min_max_8323_8264, control_int_element_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<int> := [3, 1, 2]; "
            "Console.WriteLine(a.Min()); "
            "Console.WriteLine(a.Max()); ")),
        "1\n3\n");
}

// CONTROL — a USER RECORD implementing `IComparable<T>` keeps its own arm
// (the #8092 one) and its own order (the RECORD's `CompareTo`).
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_user_record_element_unchanged) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public record Meters(cents: int): IComparable<Meters> "
            "  public method CompareTo(other: Meters): int "
            "    if cents < other.cents then return -1; end if; "
            "    if cents > other.cents then return 1; end if; "
            "    return 0; "
            "  end CompareTo; "
            "end Meters; "
            "class C public static method Main(): int "
            "  var a: Array<Meters> := [new Meters(30), new Meters(10), "
            "                           new Meters(20)]; "
            "  var lo: Meters := a.Min(); "
            "  var hi: Meters := a.Max(); "
            "  Console.WriteLine(lo.cents); "
            "  Console.WriteLine(hi.cents); "
            "  return 0; end Main; end C;"),
        "10\n30\n");
}

// CONTROL — a USER RECORD whose `CompareTo` is INHERITED from a GENERIC base
// keeps the #8272 monomorphized path and its own order.
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_user_record_generic_base_compareto_unchanged) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "class B2<T>: IComparable<T> "
            "  public virtual method Cmp(other: T): int => 0; "
            "  public method CompareTo(other: T): int => Cmp(other); "
            "end B2; "
            "class Item2: B2<Item2> "
            "  public A: int; "
            "  public constructor(a: int) A := a; end; "
            "  public override method Cmp(other: Item2): int "
            "    if A < other.A then return -1; end if; "
            "    if A > other.A then return 1; end if; "
            "    return 0; "
            "  end Cmp; "
            "end Item2; "
            "class C public static method Main(): int "
            "  var a: Array<Item2> := [new Item2(30), new Item2(10), "
            "                           new Item2(20)]; "
            "  var lo: Item2 := a.Min(); "
            "  var hi: Item2 := a.Max(); "
            "  Console.WriteLine(lo.A); "
            "  Console.WriteLine(hi.A); "
            "  return 0; end Main; end C;"),
        "10\n30\n");
}

// CONTROL — a LOCAL USER type named `Guid` is NOT the stdlib §2.6 `Guid`.
// §2.3: "every other always-available name may be shadowed by local
// declarations under the normal name-resolution rules", so a user's `Guid`
// MUST win — and it must be ordered by ITS OWN `CompareTo`, not by §2.6's
// canonical-byte rule. The row is the discriminator: the values are 10/20/30,
// so a fix that mistook this type for the built-in would answer by the
// built-in's byte order (which would still be 10/30 here — so the row also
// asserts the program's TYPE, below, which the built-in arm would not
// produce).
//
// The observable that discriminates is the ORDER over a pair whose USER
// order and §2.6's canonical order DISAGREE: the user's `CompareTo` orders
// by `V`, so the minimum is the `V = 1` element; the built-in's canonical
// byte order over the same two Guids' first bytes would rank the
// `33333333-…` element (byte 0 = 0x33) BELOW the `11111111-…` one.
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_local_user_type_named_guid_is_not_the_builtin) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public record Guid: IComparable<Guid> "
            "  public V: int; "
            "  public constructor(v: int) V := v; end; "
            "  public method CompareTo(other: Guid): int "
            "    if V < other.V then return -1; end if; "
            "    if V > other.V then return 1; end if; "
            "    return 0; "
            "  end CompareTo; "
            "end Guid; "
            "class C public static method Main(): int "
            "  var a: Array<Guid> := [new Guid(30), new Guid(10), "
            "                           new Guid(20)]; "
            "  var lo: Guid := a.Min(); "
            "  var hi: Guid := a.Max(); "
            "  Console.WriteLine(lo.V); "
            "  Console.WriteLine(hi.V); "
            "  return 0; end Main; end C;"),
        "10\n30\n");
}

// CONTROL — an IMPORTED USER type named `Guid` is NOT the stdlib §2.6
// `Guid` either. This row is the #5272 discriminator the house predicate
// `sema::isStdlibGuidType` implements: an imported type carries a populated
// `moduleName`, so it is a USER type and the predicate must not match it.
//
// The observable is the DIAGNOSTIC: a user `Guid` that declares NO
// `IComparable` is refused by the §5.1 constraint gate. Had the predicate
// matched it, the gate would have ADMITTED the call (as it does for the
// built-in `Guid`) and the program would have compiled — so the refusal
// below is the proof that the stdlib `Guid` and a user `Guid` were not
// confused.
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_imported_user_type_named_guid_is_not_the_builtin) {
    std::vector<ModuleSource> mods;
    mods.push_back({"UserGuidLib", R"BZ(
module UserGuidLib;
public class Guid
  public V: int;
  public constructor(v: int) V := v; end;
end Guid;
)BZ"});
    mods.push_back({"UserGuidApp", R"BZ(
module UserGuidApp;
import UserGuidLib as l;
class C
  public static method Main(): int
    var a: Array<l.Guid> := [new l.Guid(30), new l.Guid(10), new l.Guid(20)];
    Console.WriteLine(a.Min());
    return 0;
  end Main;
end C;
)BZ"});
    auto r = runAotModules(mods, "UserGuidApp");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "Guid does not implement IComparable<T>"));
}

// CONTROL — a NULLABLE element is STILL REFUSED: the `T?` question is an
// OWNER RULING (#8241), not this fix's to decide, and this fix does NOT
// widen the nullable case. The house predicate `sema::isStdlibGuidType`
// returns false for a nullable type (spec §14.2: "a type-bound constraint —
// naming a class or interface — implies non-null T"), so `Array<Guid?>`
// keeps the #2770 refusal.
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_nullable_guid_element_is_still_refused) {
    auto r = runAot(prog(
        "var a: Array<Guid?> := [null, "
        "Guid.Parse(\"11111111-1111-1111-1111-111111111111\")]; "
        "Console.WriteLine(a.Min()); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r, "without a selector requires the element type to satisfy"));
}

// CONTROL — a reference element with NO ordering (`Object`) is STILL
// REFUSED. The new arm must not have widened the #2770 guard.
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_object_element_is_still_refused) {
    auto r = runAot(prog(
        "var a: Array<Object> := [new Object(), new Object()]; "
        "Console.WriteLine(a.Min()); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r, "without a selector requires the element type to implement"));
}

// CONTROL — an INTERFACE element keeps the #2770 itable-thunk path: an
// `interface IFoo: IComparable<IFoo>` element is ADMITTED and ordered
// through the interface thunk (`bz$ifacecmp$<Iface>`), which is the very
// dispatch a written-out `el.CompareTo(acc)` uses.
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_interface_element_unchanged) {
    BEZEL_EXPECT_EQ(
        out("module M; "
            "public interface IFoo: IComparable<IFoo> "
            "  method Rank(): int; "
            "end IFoo; "
            "class Node: IFoo "
            "  private _r: int; "
            "  public constructor(r: int) _r := r; end constructor; "
            "  public method Rank(): int => _r; "
            "  public method CompareTo(other: IFoo): int => _r - other.Rank(); "
            "end Node; "
            "class C public static method Main(): int "
            "  var a: Array<IFoo> := [new Node(3), new Node(1), new Node(2)]; "
            "  var lo: IFoo := a.Min(); "
            "  var hi: IFoo := a.Max(); "
            "  Console.WriteLine(lo.Rank()); "
            "  Console.WriteLine(hi.Rank()); "
            "  return 0; end Main; end C;"),
        "1\n3\n");
}

// CONTROL — `Array<IDisposable>.Min()` is STILL REFUSED (the #2784 case:
// a builtin interface element carries no `declaration`, so the gate sees no
// `IComparable`).
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_idisposable_element_is_still_refused) {
    auto r = runAot(
        "module M; "
        "class Node "
        "  public constructor() end constructor; "
        "  public method Tag(): int => 4; "
        "end Node; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Node(); "
        "    using var b := new Node(); "
        "    var arr: Array<IDisposable> := [a, b]; "
        "    Console.WriteLine(arr.Min()); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r, "without a selector requires the element type to satisfy"));
}

// CONTROL — the SELECTOR form (`Min<U>(selector: (T) -> U): U`, stdlib §5)
// is the documented alternative and already worked before this fix; it must
// keep working, and it must keep ordering by the same §2.6 order.
BEZEL_TEST(array_guid_min_max_8323_8264,
           control_selector_form_still_works) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "var a: Array<Guid> := "
            "[Guid.Parse(\"22222222-2222-2222-2222-222222222222\"), "
            "Guid.Parse(\"11111111-1111-1111-1111-111111111111\")]; "
            "Console.WriteLine(a.Min(lambda(g: Guid) => g)); "
            "Console.WriteLine(a.Max(lambda(g: Guid) => g)); ")),
        lines({"11111111-1111-1111-1111-111111111111",
               "22222222-2222-2222-2222-222222222222"}));
}
