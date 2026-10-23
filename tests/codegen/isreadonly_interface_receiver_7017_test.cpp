// Codegen — `IsReadOnly` read through a BUILTIN COLLECTION INTERFACE receiver
// (#7017; stdlib §4.1 / §4.2, language spec §27.6, v0.340).
//
// THE DEFECT. stdlib §4.1 declares the member on `ICollection<T>`:
//
//   public interface ICollection<T>: IReadOnlyCollection<T>
//     readonly property IsReadOnly: bool;
//
// and §4.2 declares the same member on `IOwningCollection<T>`; `IList<T>`
// (§4.1) and `IOwningList<T>` (§4.2) inherit it. The member is DECLARED, so
// the #6997 sema arm (which rejects a bare reference to a name a head does
// NOT declare) admits it — and nothing lowered it:
//
//     var a: Array<int> := [1,2,3];
//     var co: ICollection<int> := a;
//     var r: bool := co.IsReadOnly;   // #2434: degraded to a zero value
//     Console.WriteLine(r);           // "False" — but by ACCIDENT
//
// The local degraded to a zero-initialised value and the program answered
// `False` — which is the CORRECT value, so the annotated form's OUTPUT does
// not distinguish fixed from unfixed. What distinguishes them is the
// `internal: … — #2434` Warning, and the two UNANNOTATED / DIRECT forms whose
// output DOES differ:
//
//   * `Console.WriteLine(co.IsReadOnly)` — pre-fix the argument produced no
//     value, so the #3079 silent no-op fired and the statement emitted
//     NOTHING, not even the line terminator;
//   * `var f := il.IsReadOnly;` — pre-fix the local was untyped, so it held
//     the zero value and printed `0`, not `False`.
//
// The lit case tests/lit/isreadonly_interface_receiver_no_degrade_7017.bzl
// pins the warning's ABSENCE on the issue's own repro, which is the only
// mechanical oracle for the annotated form (runAot discards compiler stderr
// on a successful compile — see the #5012/#5013 lit case's header for the
// same reasoning).
//
// THE VALUE. §4.1 fixes it outright: "`IsReadOnly` is `false` for every
// standard collection implementing `ICollection<T>` (`Array<T>`,
// `LinkedList<T>`, `Set<T>`, `Dictionary<K, V>` …). The standard library
// currently ships no read-only `ICollection<T>` implementation; the member
// exists for user-defined read-only implementations, which return `true`".
// §4.2's owning pair is implemented by the owning standard collections
// (OwningArray §4.5, OwningLinkedList §4.7, OwningSet §4.10), whose
// `IsReadOnly` the #4746 arm already answers `false`.
//
// The "user-defined read-only implementation" §4.1 names CAN reach such a
// receiver — `public class MyList: IList<int> … end MyList;` with `var il:
// IList<int> := new MyList();` is accepted (§5.1) — and through THAT receiver
// the constant is wrong: the head erases the concrete layout with no itable,
// so there is nothing to dispatch through. MEASURED on this lane's base
// (5c3e10f7), over a `MyList` whose `_a` is `[1,2,3]` and whose `IsReadOnly`
// returns `true`: `il.Count` → `0`, `il.Contains(2)` → `False`,
// `il.IsReadOnly` → `False` — all three wrong, all three with no diagnostic.
// That family is #7063; the rows below cover the standard collections, which
// §4.1 and §4.2 pin to `false`. The pre-fix #2434 fallback printed `False`
// for the user-implementation receiver too, so this fix is not a regression
// there. The other available answer — an Error — is excluded, because §4.1
// DECLARES the member on these heads and §27.6 resolves a bare reference
// "on the value exactly as the call `v.Scaled()` is"; rejecting it would turn
// an accepted program into a rejection.
//
// Also NOT this arm's case, but for a DIFFERENT reason than the paragraph
// above: a bare type parameter constrained by `where T: ICollection<int>`
// DOES expose `IsReadOnly`. §3.6 rule 2 / §14.2 puts the head's members in
// T's surface inside the generic body, and stdlib §4.1 DECLARES
// `readonly property IsReadOnly: bool;` on `ICollection<T>` (§4.2 declares
// the same on `IOwningCollection<T>`), so the read is ADMITTED and TYPED
// `bool` through the CONSTRAINT path — the #7086 fix, which established
// these heads' documented surface for a constrained type parameter. The
// statement that a constrained bare type parameter "exposes no `IsReadOnly`
// member at all" was true of the PRE-#7086 compiler and is FALSE now.
//
// The two paths stay distinct, so the read does NOT reach this arm. This
// arm is the INTERFACE-TYPED RECEIVER — a value whose STATIC type is the
// head (`var co: ICollection<int> := a; co.IsReadOnly`), where the head
// erases the concrete collection and this arm supplies the constant. The
// constraint path's receiver is a bare type PARAMETER, and codegen resolves
// it to the type it is SUBSTITUTED with before the arm dispatch
// (`resolveTypeParamRef`, LlvmBackend_Exprs.cpp's #7086 change), so the
// read is answered by the arm for that substituted type — the
// CONCRETE-collection arm for an `Array<int>` argument, this arm for an
// interface-typed one. Both answer the same §4.1 constant. The read is also
// TYPED by sema on the constraint path (§14.2's surface), so this arm's
// codegen type-patch is not needed for it.
//
// MEASURED on this lane's tip: `where T: ICollection<int>` and `where T:
// IList<int>`, each reading `x.IsReadOnly` over an `Array<int>` argument,
// both print "rw" (the §4.1 value `false`) and compile clean. The two
// `internal:` diagnostics are the generic TEMPLATE body's placeholder
// lowering — the dead body has no substitution, so its bare `T` matches no
// arm. The SPECIALIZATION answers the read from the concrete type: the same
// program's `x.Count` prints 3, not 0 (a degraded read would print 0 and
// would not print the line at all — the #3079 no-op).
//
// ORACLE. `runAot` / `runAotOut`. Every positive row asserts the program's
// EXACT stdout, so a statement the #3079 no-op deleted is a failure (an empty
// stdout is exactly the pre-fix behaviour).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// stdlib §4.1: `ICollection<T>` declares `IsReadOnly`; the value is `false`
// for every standard collection implementing it.
const char* kCollectionInterfaceIsReadOnly =
    "module M; "
    "class C public static method Main() "
    "  var a: Array<int> := [1,2,3]; "
    "  var co: ICollection<int> := a; "
    "  var r: bool := co.IsReadOnly; "
    "  Console.WriteLine(r); "
    "end Main; end C;";

// stdlib §4.1: `IList<T>` inherits `IsReadOnly` from `ICollection<T>`.
const char* kListInterfaceIsReadOnly =
    "module M; "
    "class C public static method Main() "
    "  var a: Array<int> := [1,2,3]; "
    "  var il: IList<int> := a; "
    "  var r: bool := il.IsReadOnly; "
    "  Console.WriteLine(r); "
    "end Main; end C;";

// stdlib §4.2: `IOwningCollection<T>` declares `IsReadOnly` itself.
const char* kOwningCollectionInterfaceIsReadOnly =
    "module M; "
    "class E public N: int := 0; end E; "
    "class C public static method Main() "
    "  using var oc: IOwningCollection<E> := new OwningArray<E>(); "
    "  var r: bool := oc.IsReadOnly; "
    "  Console.WriteLine(r); "
    "end Main; end C;";

}  // namespace

// The issue's own repro, verbatim. NOTE: the OUTPUT of this row does NOT
// distinguish fixed from unfixed — the degraded zero value and the member's
// value are both `false` (§4.1). The row pins the VALUE; the lit case
// tests/lit/isreadonly_interface_receiver_no_degrade_7017.bzl pins the
// ABSENCE of the #2434 degradation, which is the defect itself.
BEZEL_TEST(isreadonly_interface_receiver_7017,
           collection_interface_isread_only_prints_false) {
    BEZEL_EXPECT_EQ(runAotOut(kCollectionInterfaceIsReadOnly), "False");
}

// stdlib §4.1: `IList<T>` inherits the member from `ICollection<T>`.
BEZEL_TEST(isreadonly_interface_receiver_7017,
           list_interface_isread_only_prints_false) {
    BEZEL_EXPECT_EQ(runAotOut(kListInterfaceIsReadOnly), "False");
}

// stdlib §4.2: `IOwningCollection<T>` declares the member itself, and the
// owning standard collections answer `false`.
BEZEL_TEST(isreadonly_interface_receiver_7017,
           owning_collection_interface_isread_only_prints_false) {
    BEZEL_EXPECT_EQ(runAotOut(kOwningCollectionInterfaceIsReadOnly), "False");
}

// The read as a DIRECT `Console.WriteLine` argument. Pre-fix the argument
// produced no value, so the #3079 silent no-op fired and the statement
// emitted NOTHING — an empty stdout, which this row's exact-match
// assertion rejects. This is the row that fails without the fix.
BEZEL_TEST(isreadonly_interface_receiver_7017,
           direct_isread_only_argument_is_not_silently_dropped) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  var a: Array<int> := [1,2,3]; "
        "  var co: ICollection<int> := a; "
        "  Console.WriteLine(co.IsReadOnly); "
        "end Main; end C;"), "False");
}

// The read through the OWNING interface, as a direct argument. Pre-fix this
// was the #3079 no-op too (the whole statement vanished).
BEZEL_TEST(isreadonly_interface_receiver_7017,
           direct_isread_only_argument_on_owning_interface_is_not_dropped) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class E public N: int := 0; end E; "
        "class C public static method Main() "
        "  using var oc: IOwningCollection<E> := new OwningArray<E>(); "
        "  Console.WriteLine(oc.IsReadOnly); "
        "end Main; end C;"), "False");
}

// The INFERRED local (`var f := il.IsReadOnly;` — no annotation). Pre-fix the
// read was untyped, so the local held the zero value and printed `0`; the
// member's value is `false` (§4.1), which renders as `False`. This row is
// the one that fails without the fix on the OUTPUT alone.
//
// (This shape is the R35 follow-up list's H11 row — "`var f := il.IsReadOnly`
// unannotated over `IList` prints `0` + #2434 degrade; spec `False`" — which
// the R34 reviewer judged NOT covered by #7017's annotated repro. It is the
// same codegen arm, so this fix covers it; a comment on #7017 records the
// measurement.)
BEZEL_TEST(isreadonly_interface_receiver_7017,
           inferred_local_isread_only_holds_the_members_value) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  var a: Array<int> := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  var f := il.IsReadOnly; "
        "  Console.WriteLine(f); "
        "end Main; end C;"), "False");
}

// The read is a `bool` VALUE, not merely something `Console.WriteLine` can
// format: it binds to an inferred local and drives a condition (spec §20).
// A lowering that produced no value could not reach either position.
BEZEL_TEST(isreadonly_interface_receiver_7017,
           isread_only_binds_to_a_bool_local_and_drives_a_condition) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  var a: Array<int> := [1,2,3]; "
        "  var co: ICollection<int> := a; "
        "  var ro := co.IsReadOnly; "
        "  Console.WriteLine((if ro then \"read-only\" else \"mutable\")); "
        "end Main; end C;"), "mutable");
}

// The #3079 degrade DROPS THE WHOLE STATEMENT — "not even the line
// terminator" — so a program that prints a marker either side of the read
// distinguishes "the statement ran and printed False" from "the statement was
// silently deleted". Pre-fix this program compiled at exit 0 and printed
// exactly "before\nafter".
BEZEL_TEST(isreadonly_interface_receiver_7017,
           the_writeline_statement_is_not_silently_dropped) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main() "
        "  Console.WriteLine(\"before\"); "
        "  var a: Array<int> := [1,2,3]; "
        "  var co: ICollection<int> := a; "
        "  Console.WriteLine(co.IsReadOnly); "
        "  Console.WriteLine(\"after\"); "
        "end Main; end C;"), "before\nFalse\nafter");
}
