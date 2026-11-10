// Codegen + sema regression — issue #8260: `x in lst` where `lst` is an
// `IList<T>` (or another INTERFACE receiver) was refused by codegen with
// "not yet implemented: codegen for this `in` expression form".
//
// WHAT THE DOCUMENTS REQUIRE.
//
// spec §17.5 (v0.355) — the `in` operator:
//   "`x in collection` evaluates to `true` if `collection.Contains(x)` is
//    true. The right operand must expose a `Contains(T): bool` method (instance
//    or extension)."
//   "The `Contains` lookup is structural rather than interface-based: any
//    accessible method named `Contains` taking one parameter of compatible type
//    and returning `bool` qualifies."
//
// stdlib §4.1 (v0.355) — the borrowing collection interfaces:
//   `public interface ICollection<T>: IReadOnlyCollection<T>`
//     `method Contains(item: T): bool;` …
//   `public interface IList<T>: IReadOnlyList<T>, ICollection<T>`
// so `IList<T>.Contains` IS `ICollection<T>.Contains`.
//
// stdlib §4 (*Equality search at an element type with no equality*, v0.355) —
// the §14.2 admission condition of that member, stated for the OPERATOR too:
//   "There the call is a compile error, `x in xs` (language spec §17.5) and a
//    method reference formed over the member (language spec §27.6) included".
//
// WHAT WAS WRONG (MEASURED on r30-c1, the base of this lane, with a compiler
// built at that tip — #7598 included).
//
//   not yet implemented: codegen for this `in` expression form
//
// — for `3 in lst` over an `IList<int>`, while `lst.Contains(3)` compiled and
// answered `True`. The same refusal for `3 in c` over an `ICollection<int>`,
// over an `IReadOnlyCollection<int>`, and over a user class that declares its
// own `Contains`. The left operand was never TYPE-checked at an interface
// receiver either: `1.5 in lst` over an `IList<int>` was blamed on the
// lowering layer rather than on §5.1.
//
// THE ROWS.
//
//  (A) The interface receiver works and answers the DATA: `IList<int>` and
//      `IReadOnlyList<int>` — the two borrowing LIST interfaces, the sound
//      subset #853 bridges to the array runtime — answer `True`/`False` for a
//      present and an absent element, over an int, a string, a record and a
//      Guid element, and under `not`.
//  (B) The `Array<int>` control still answers the same verdicts.
//  (C) The polymorphic COLLECTION interfaces keep the #853 refusal — the
//      diagnostic is BYTE-IDENTICAL to the one `c.Contains(x)` gets, which is
//      the point: `x in c` IS `c.Contains(x)`.
//  (D) A USER class receiver is NOT admitted by THIS fix (#8260): the review of
//      a77442ad measured the user-class arm accepting a `private` Contains from
//      another class, §13.1, and a nullable receiver, §7.4, which crashed at
//      run time. Those programs keep the base's refusal; the rows pin it.
//      ⇒ #8325 LATER moved this receiver family: §17.5's STRUCTURAL lookup
//      now decides it in SEMA, so a user receiver with an ACCESSIBLE instance
//      `Contains` COMPILES, and the two refusals the review measured are
//      refusals with a rule NAMED at the `in` site (§13.1's accessibility,
//      §7.4's narrowing) instead of the backend's "not yet implemented".
//      The four (D) rows below are the #8325 flip: the other rows of this
//      file — the interface family, the control, the #853 refusals, the
//      function-typed element, the §5.1 left-operand check — are untouched.
//  (G) A NULLABLE interface receiver gets the written call's §7.4 refusal.
//  (E) The function-typed element follows the `Contains` call EXACTLY (#5520 /
//      #7598): ADMITTED at an `IList` receiver (`IList<T>.Contains` is stdlib
//      §4's equality search, and a function value HAS a §9.2 first-kind
//      `Equals` — §27.7.1), REFUSED at an `IReadOnlyList` one (that head
//      declares no `Contains`, so §5's `where T: hashable` governs it).
//  (F) The left operand's TYPE is checked at an interface receiver: `1.5 in
//      lst` over an `IList<int>` is refused with §5.1's message, exactly as
//      `lst.Contains(1.5)` is.
//
// ORACLE NOTES.  `runAotChecked` drives the real AOT path (fork-isolated
// `Driver::compileFile` + clang link + native exec) under ASan/LSan. The rows
// read the program's DATA (the membership verdicts), not a constant.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// True when some diagnostic in `r` carries `needle`.
bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// (A) The borrowing LIST interfaces — `IList<T>` and `IReadOnlyList<T>`.
//
// `arr` is the concrete `Array<T>`; the interface-typed local is a BORROW of
// the same runtime value, so the interface row and the `Array` control row
// read the same data.
// ---------------------------------------------------------------------------
const char* kIListInt =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<int> := new Array<int>(); "
    "    arr.Add(3); arr.Add(7); "
    "    var lst: IList<int> := arr; "
    "    Console.WriteLine(3 in lst); "
    "    Console.WriteLine(4 in lst); "
    "    Console.WriteLine(7 in lst); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kIReadOnlyListInt =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<int> := new Array<int>(); "
    "    arr.Add(3); arr.Add(7); "
    "    var rol: IReadOnlyList<int> := arr; "
    "    Console.WriteLine(3 in rol); "
    "    Console.WriteLine(4 in rol); "
    "    Console.WriteLine(7 in rol); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The string element — the CONTENT comparison (§4.6), not a pointer compare.
const char* kIListString =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<string> := new Array<string>(); "
    "    arr.Add(\"alpha\"); arr.Add(\"beta\"); "
    "    var lst: IList<string> := arr; "
    "    Console.WriteLine(\"alpha\" in lst); "
    "    Console.WriteLine(\"zzz\" in lst); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The string element, LOOPED 100x. `0..100` is INCLUSIVE (§16.3), so the loop
// runs 101 times — the count below is that count, not 100.
const char* kIListStringLooped =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<string> := new Array<string>(); "
    "    arr.Add(\"alpha\"); arr.Add(\"beta\"); "
    "    var lst: IList<string> := arr; "
    "    var hits := 0; "
    "    for var i in 0..100 do "
    "      if \"beta\" in lst then hits := hits + 1; end if; "
    "    end for; "
    "    Console.WriteLine(hits); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The record element — the structural equality of §8.3 / §9.1.
const char* kIListRecord =
    "module M; "
    "public record Point(x: int, y: int) end Point; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<Point> := new Array<Point>(); "
    "    arr.Add(new Point(1, 2)); "
    "    var lst: IList<Point> := arr; "
    "    var p := new Point(1, 2); "
    "    var q := new Point(3, 4); "
    "    Console.WriteLine(p in lst); "
    "    Console.WriteLine(q in lst); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The Guid element — stdlib §2.6's value record.
const char* kIListGuid =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<Guid> := new Array<Guid>(); "
    "    arr.Add(Guid.Empty); "
    "    var lst: IList<Guid> := arr; "
    "    Console.WriteLine(Guid.Empty in lst); "
    "    Console.WriteLine(Guid.NewGuid() in lst); "
    "    return 0; "
    "  end Main; "
    "end C;";

// `not (x in lst)` — §17.5's operator under the unary `not`.
const char* kIListNot =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<int> := new Array<int>(); "
    "    arr.Add(3); "
    "    var lst: IList<int> := arr; "
    "    Console.WriteLine(not (3 in lst)); "
    "    Console.WriteLine(not (4 in lst)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// (B) The `Array<int>` control — the shape that already worked (#8260 is not
// about it, and it must keep working).
// ---------------------------------------------------------------------------
const char* kArrayIntControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<int> := new Array<int>(); "
    "    arr.Add(3); arr.Add(7); "
    "    Console.WriteLine(3 in arr); "
    "    Console.WriteLine(4 in arr); "
    "    Console.WriteLine(7 in arr); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// (C) The polymorphic COLLECTION interfaces keep the #853 refusal — and the
// refusal is the one `c.Contains(x)` gets, byte for byte.
// ---------------------------------------------------------------------------
const char* kICollectionRefused =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<int> := new Array<int>(); "
    "    arr.Add(3); "
    "    var c: ICollection<int> := arr; "
    "    var b := 3 in c; "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kIReadOnlyCollectionRefused =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<int> := new Array<int>(); "
    "    arr.Add(3); "
    "    var c: IReadOnlyCollection<int> := arr; "
    "    var b := 3 in c; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// (D) USER-class receivers. #8260 kept them all refused; #8325 moved this
// receiver family to §17.5's STRUCTURAL `Contains` lookup, decided in SEMA
// (spec §17.5, v0.355 — "any accessible method named `Contains` taking one
// parameter of compatible type and returning `bool` qualifies, whether it is
// an instance member or an extension method"). So:
//   * an ACCESSIBLE instance `Contains` on a non-null receiver COMPILES and
//     answers the data;
//   * a `private Contains` reached from another class is refused with §13.1's
//     accessibility, NAMED at the `in` site (the review of a77442ad measured
//     that arm ACCEPTING it);
//   * a NULLABLE receiver that §7.4 has not narrowed is refused with §7.4's
//     narrowing remedies, NAMED at the `in` site (the review measured that
//     arm CRASHING at run time on `Q(null)`).
// ---------------------------------------------------------------------------
const char* kUserClassPublicContainsAdmitted =
    "module M; "
    "public class Bag "
    "  private owned _items: Array<int>; "
    "  public constructor() _items := new Array<int>(); end constructor; "
    "  public method Contains(item: int): bool "
    "    return _items.Contains(item); "
    "  end Contains; "
    "end Bag; "
    "class C "
    "  public static method Main(): int "
    "    var b := new Bag(); "
    "    var r := 1 in b; "
    "    return 0; "
    "  end Main; "
    "end C;";

// p06: a PRIVATE Contains called from another class.
const char* kUserClassPrivateContains =
    "module P; "
    "public class Bag "
    "  private method Contains(item: int): bool return true; end Contains; "
    "end Bag; "
    "class C "
    "  public static method Main(): int "
    "    var b := new Bag(); "
    "    Console.WriteLine(3 in b); "
    "    return 0; "
    "  end Main; "
    "end C;";

// p34: a NULLABLE user-class receiver whose Contains reads a field (the
// accepted program segfaulted on Q(null)).
const char* kUserClassNullableReceiverCrash =
    "module P; "
    "public class Bag "
    "  private owned _items: Array<int>; "
    "  public constructor() _items := new Array<int>(); end constructor; "
    "  public method Contains(item: int): bool "
    "    return _items.Contains(item); end Contains; "
    "end Bag; "
    "class C "
    "  public static method Q(b: Bag?): bool "
    "    return 7 in b; "
    "  end Q; "
    "  public static method Main(): int "
    "    Console.WriteLine(\"before\"); "
    "    Console.WriteLine(Q(null)); "
    "    Console.WriteLine(\"after\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// p31: the same with a body that does not read `this` (printed True for null).
const char* kUserClassNullableReceiverNoThis =
    "module P; "
    "public class Bag "
    "  public constructor() end constructor; "
    "  public method Contains(item: int): bool return item = 7; end Contains; "
    "end Bag; "
    "class C "
    "  public static method Q(b: Bag?): bool "
    "    return 7 in b; "
    "  end Q; "
    "  public static method Main(): int "
    "    var b := new Bag(); "
    "    Console.WriteLine(Q(b)); "
    "    Console.WriteLine(Q(null)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// (G) A NULLABLE interface receiver: `b.Contains(7)` is refused under §7.4,
// and `7 in b` IS `b.Contains(7)` (§17.5).
// ---------------------------------------------------------------------------
// p32
const char* kNullableIListReceiver =
    "module P; "
    "class C "
    "  public static method Q(b: IList<int>?): bool "
    "    return 7 in b; "
    "  end Q; "
    "  public static method Main(): int "
    "    var a := new Array<int>(); "
    "    a.Add(7); "
    "    Console.WriteLine(Q(a)); "
    "    Console.WriteLine(Q(null)); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kNullableIReadOnlyListReceiver =
    "module P; "
    "class C "
    "  public static method Q(b: IReadOnlyList<int>?): bool "
    "    return 7 in b; "
    "  end Q; "
    "  public static method Main(): int "
    "    Console.WriteLine(Q(null)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The narrowed form stays admitted, as the written call is (§7.4).
const char* kNarrowedIListReceiver =
    "module P; "
    "class C "
    "  public static method Q(b: IList<int>?): bool "
    "    if b <> null then return 7 in b; end if; "
    "    return false; "
    "  end Q; "
    "  public static method Main(): int "
    "    var a := new Array<int>(); "
    "    a.Add(7); "
    "    Console.WriteLine(Q(a)); "
    "    Console.WriteLine(Q(null)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// (E) The function-typed element follows the `Contains` call EXACTLY.
//
// At an `IList` receiver `IList<T>.Contains` IS stdlib §4's equality search
// (#7598), and a function value HAS a §9.2 first-kind `Equals` (§27.7.1: "A
// lambda (closure) value equals only itself") — so the search is ADMITTED and
// answers `True` for the stored element and `False` for a SEPARATELY-
// CONSTRUCTED lambda with identical source text.
//
// At an `IReadOnlyList` receiver `IReadOnlyList<T>` declares NO `Contains`
// (stdlib §4.1), so its `Contains` is §5's `where T: hashable` extension
// operator and a function element is REFUSED — the very refusal
// `rol.Contains(f)` gets.
//
// ORACLE NOTE.  The ADMITTED row runs on `runAot`, not `runAotChecked`: a
// closure held in a collection leaks its 64-byte heap block under LSan, and
// that leak is PRE-EXISTING and NOT this lane's — MEASURED, the already-
// working `f in arr` shape (the `Array` receiver the operator has always
// lowered) leaks the same 64 bytes under the same ASan/LSan link, and the
// written `lst.Contains(f)` spelling leaks 128 (both closures). The #7598
// file, which pins the same `IList<fn>.Contains` admission, likewise uses
// `runAot`. Every other row in this file is on `runAotChecked`.
// ---------------------------------------------------------------------------
const char* kIListFunctionElement =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var f: (int) -> int := lambda(x: int) => x + 1; "
    "    var g: (int) -> int := lambda(x: int) => x + 1; "
    "    var lst: IList<(int) -> int> := new Array<(int) -> int>(); "
    "    lst.Add(f); "
    "    Console.WriteLine(f in lst); "
    "    Console.WriteLine(g in lst); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kIReadOnlyListFunctionElementRefused =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var f: (int) -> int := lambda(x: int) => x + 1; "
    "    var arr: Array<(int) -> int> := new Array<(int) -> int>(); "
    "    arr.Add(f); "
    "    var rol: IReadOnlyList<(int) -> int> := arr; "
    "    var b := f in rol; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// (F) The left operand's TYPE is checked at an interface receiver — the same
// §5.1 refusal `lst.Contains(1.5)` gets.
// ---------------------------------------------------------------------------
const char* kIListLeftOperandType =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var arr: Array<int> := new Array<int>(); "
    "    arr.Add(3); "
    "    var lst: IList<int> := arr; "
    "    var b := 1.5 in lst; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------------
// (A) the interface receiver works and answers the DATA.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           ilist_int_element_membership_answers_the_data) {
    JitResult r = runAotChecked(kIListInt);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nTrue\n");
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           ireadonlylist_int_element_membership_answers_the_data) {
    JitResult r = runAotChecked(kIReadOnlyListInt);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nTrue\n");
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           ilist_string_element_membership_compares_content) {
    JitResult r = runAotChecked(kIListString);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           ilist_string_element_membership_looped_100x) {
    JitResult r = runAotChecked(kIListStringLooped);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "101\n");
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           ilist_record_element_membership_is_structural) {
    JitResult r = runAotChecked(kIListRecord);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           ilist_guid_element_membership_answers_the_data) {
    JitResult r = runAotChecked(kIListGuid);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           not_over_an_interface_membership_inverts_the_verdict) {
    JitResult r = runAotChecked(kIListNot);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\n");
}

// ---------------------------------------------------------------------------
// (B) the `Array<int>` control.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           array_int_control_still_answers_the_data) {
    JitResult r = runAotChecked(kArrayIntControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nTrue\n");
}

// ---------------------------------------------------------------------------
// (C) the polymorphic collection interfaces keep the #853 refusal — the SAME
// one `c.Contains(x)` gets.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           icollection_receiver_keeps_the_853_refusal) {
    JitResult r = runAotChecked(kICollectionRefused);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "`.Contains` through a polymorphic collection interface"));
    BEZEL_EXPECT_TRUE(hasError(r, "#853"));
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           ireadonlycollection_receiver_keeps_the_853_refusal) {
    JitResult r = runAotChecked(kIReadOnlyCollectionRefused);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "`.Contains` through a polymorphic collection interface"));
    BEZEL_EXPECT_TRUE(hasError(r, "#853"));
}

// ---------------------------------------------------------------------------
// (D) USER-class receivers — the #8325 flip. The four rows below are the
// ones #8325's §17.5 structural lookup moved: the first now COMPILES (an
// accessible instance `Contains` on a non-null receiver), and the other three
// are refused by the rule they break, NAMED at the `in` site — the backend's
// "not yet implemented" message is gone from this receiver family.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           user_class_receiver_is_admitted_p8325) {
    JitResult r = runAot(kUserClassPublicContainsAdmitted);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           user_class_private_contains_is_not_accessible_p06) {
    JitResult r = runAotChecked(kUserClassPrivateContains);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // §13.1 — the structural lookup qualifies an ACCESSIBLE member only, and
    // the rule is NAMED at the `in` site.
    BEZEL_EXPECT_TRUE(hasError(r, "is not accessible here"));
    BEZEL_EXPECT_TRUE(hasError(r, "13.1"));
    // The backend's layer is gone.
    BEZEL_EXPECT_FALSE(hasError(
        r, "not yet implemented: codegen for this `in` expression form"));
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           user_class_nullable_receiver_is_refused_p34) {
    JitResult r = runAotChecked(kUserClassNullableReceiverCrash);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // §7.4 — the written call's refusal, at the `in` site.
    BEZEL_EXPECT_TRUE(hasError(
        r, "member access `.Contains` on a nullable value requires "
           "narrowing it to non-null first (§7.4)"));
    BEZEL_EXPECT_FALSE(hasError(
        r, "not yet implemented: codegen for this `in` expression form"));
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           user_class_nullable_receiver_no_this_is_refused_p31) {
    JitResult r = runAotChecked(kUserClassNullableReceiverNoThis);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "member access `.Contains` on a nullable value requires "
           "narrowing it to non-null first (§7.4)"));
    BEZEL_EXPECT_FALSE(hasError(
        r, "not yet implemented: codegen for this `in` expression form"));
}

// ---------------------------------------------------------------------------
// (G) a nullable interface receiver gets the written call's §7.4 refusal.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           nullable_ilist_receiver_is_refused_p32) {
    JitResult r = runAotChecked(kNullableIListReceiver);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r, "member access `.Contains` on a nullable value requires "
           "narrowing it to non-null first (§7.4)"));
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           nullable_ireadonlylist_receiver_is_refused) {
    JitResult r = runAotChecked(kNullableIReadOnlyListReceiver);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r, "member access `.Contains` on a nullable value requires "
           "narrowing it to non-null first (§7.4)"));
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           narrowed_ilist_receiver_is_admitted) {
    JitResult r = runAotChecked(kNarrowedIListReceiver);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// ---------------------------------------------------------------------------
// (E) the function-typed element follows the `Contains` call EXACTLY.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           ilist_function_element_is_admitted_as_an_equality_search) {
    JitResult r = runAot(kIListFunctionElement);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           ireadonlylist_function_element_is_refused_like_the_call) {
    JitResult r = runAotChecked(kIReadOnlyListFunctionElementRefused);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "requires `T: hashable`"));
}

// ---------------------------------------------------------------------------
// (F) the left operand's type is checked at an interface receiver.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_in_over_interface_receiver_8260,
           ilist_left_operand_type_is_checked) {
    JitResult r = runAotChecked(kIListLeftOperandType);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(
        r, "the left operand of `in` over `IList` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
}
