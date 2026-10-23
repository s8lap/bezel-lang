// Sema — #7086 / §14.2 (v0.340) + stdlib §4.1, §4.2, §4.12, §4.13 + §3.6
// rule 2.
//
// THE DEFECT. A `where` bound naming a BUILTIN COLLECTION interface
// established NO members, so a call to a member that interface DOES declare
// was rejected:
//
//     public static method G<T>(x: T): int where T: IList<int>
//       x.Add(1);          // REJECTED: "no member `Add` on bare type
//                           // parameter `T`"
//       return 0;
//
// THE RULE. §14.2 (v0.340), *Interface conformance*: "Inside the generic
// body, T's surface includes IFoo's members, regardless of which conformance
// category T belongs to." `IList<int>`'s members are `Count`, `IsReadOnly`,
// `Add`, `Clear`, `Contains`, `Remove`, `Insert`, `GetEnumerator` and the
// indexer (stdlib §4.1), so the body above may call `x.Add(1)`, and the
// compiler must type it.
//
// WHY IT WAS A DEFECT AND NOT A POLICY. The compiler already does this for
// the other builtin interfaces — `where T: IComparable<int>` + `x.CompareTo(o)`
// and `where T: IEquatable<int>` + `x.Equals(o)` are ACCEPTED — because those
// two carry a REAL synthesized Interface TypeSymbol
// (`SemaContext::isSynthesizedBuiltinInterfaceName`). The eight builtin
// COLLECTION interfaces carry NO declaration and therefore no member table, so
// the constraint walk (`ExprTC::forEachConstraintBound`, which yields a bound
// only when `ctr->declaration` is non-null) could not see them.
//
// THE FIX. The constraint walk now also reaches the declaration-less bounds,
// and the stdlib's OWN documented surface (the same table #6997 / #7015 /
// #7027 reject an UNDECLARED name against) decides what the bound establishes.
// A USER interface that INHERITS the builtin one (`interface MyI: IList<int>`)
// reaches it through the declaration-less base refs
// (`TypeSymbol::nonSymbolBaseRefs`).
//
// THE OTHER SIDE. The surface is PER HEAD, so the rejections stay: a name the
// head does NOT declare is still refused — `x.Nope(1)` on `IList<int>`,
// `x.IndexOf(2)` on `IList<int>` (stdlib §4.1 declares `IndexOf` on
// `Array<T>`, §4.4, not on any collection interface), `x.Add(1)` on
// `IReadOnlyList<int>` (§4.1 declares no `Add` there), and `x.Insert(0, 1)` on
// `ICollection<int>` (`Insert` is `IList<T>`'s alone). An unconstrained `T`
// is refused exactly as before.
//
// THREE MORE REFUSALS, each MEASURED. The METHOD-REFERENCE form (§27.6,
// `var g: (int) -> bool := x.Contains;`) stays refused — #7014 ruled that the
// reference form over a collection head is not emitted (its call form is
// fenced, so the two must agree), and the reference form over the array-backed
// list heads is #7014's own deferral. The OWNING enumerator (stdlib §4.13)
// stays refused: admitting it SEGFAULTs (exit 139). And the enumeration call
// whose substitution is a LIST INTERFACE stays refused — the collection-
// interface heads erase the concrete backing, so there is no enumerator to
// build; the non-generic spelling of the same shape is rejected the same way
// (#2457), and admitting it SEGFAULTs (exit 139) too.
//
// THE VALUES. Every accepted row asserts the program's EXACT stdout, so a
// member that was admitted but not lowered (a degraded slot) fails the row.
// `x.Add(7)` really mutates the collection the parameter stands for, and
// `x.Count` really reads the count.
//
// ORACLE. `runAot`. The reject rows assert the diagnostic by needle AND that
// it was a compile-time reject with a real location, so a crash or an
// unrelated compile error cannot pass. The accept rows assert EXACT stdout.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Assert the program is REJECTED at compile time with `needle` in the message.
void expectRejected(const std::string& prog, const std::string& needle) {
    JitResult r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle.c_str()));
}

// Assert the program COMPILES, runs to completion, and prints exactly `out`.
void expectStdout(const std::string& prog, const std::string& out) {
    JitResult r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// A generic method over a bare type parameter `T` constrained to `bound`,
// whose body is `body`, called with an `Array<int>` of three elements.
std::string genericOver(const std::string& bound, const std::string& body,
                        const std::string& extra = "") {
    return "module M7086; " + extra +
           "class C public static method G<T>(x: T): int where T: " + bound +
           " " + body + " end G; "
           "public static method Main(): int "
           "var a: Array<int> := [1, 2, 3]; "
           "Console.WriteLine(G(a)); "
           "return 0; end Main; end C;";
}

}  // namespace

// ===========================================================================
// THE ADMISSIONS — §14.2's "T's surface includes IFoo's members".
// ===========================================================================

// ROW 1 — stdlib §4.1: `IList<T>` declares `Add` (inherited from
// `ICollection<T>`). The call is admitted, and it really mutates the
// collection the parameter stands for: the count reads 4, not 3.
BEZEL_TEST(constraint_collection_member_7086,
           ilist_add_call_is_admitted_and_mutates_spec_4_1) {
    expectStdout(genericOver("IList<int>", "x.Add(7); return x.Count;"),
                 "4\n");
}

// ROW 2 — stdlib §4.1: `IList<T>` declares `Insert(index: int, item: T)`.
BEZEL_TEST(constraint_collection_member_7086,
           ilist_insert_call_is_admitted_and_mutates_spec_4_1) {
    expectStdout(genericOver("IList<int>", "x.Insert(0, 7); return x.Count;"),
                 "4\n");
}

// ROW 3 — stdlib §4.1: `IList<T>` declares `Count` (inherited from
// `IReadOnlyCollection<T>`). The read answers the member's real value.
BEZEL_TEST(constraint_collection_member_7086,
           ilist_count_read_is_admitted_spec_4_1) {
    expectStdout(genericOver("IList<int>", "return x.Count;"), "3\n");
}

// ROW 4 — stdlib §4.1: `IReadOnlyList<T>` declares `Count` too.
BEZEL_TEST(constraint_collection_member_7086,
           ireadonlylist_count_read_is_admitted_spec_4_1) {
    expectStdout(genericOver("IReadOnlyList<int>", "return x.Count;"), "3\n");
}

// ROW 5 — stdlib §4.1: `IReadOnlyCollection<T>` declares `Count`.
BEZEL_TEST(constraint_collection_member_7086,
           ireadonlycollection_count_read_is_admitted_spec_4_1) {
    expectStdout(genericOver("IReadOnlyCollection<int>", "return x.Count;"),
                 "3\n");
}

// ROW 6 — stdlib §4.1: `ICollection<T>` declares `Contains(item: T): bool`.
// The call is admitted AND typed `bool` — the answer is the member's own.
BEZEL_TEST(constraint_collection_member_7086,
           icolllection_contains_call_is_admitted_spec_4_1) {
    expectStdout(
        genericOver("ICollection<int>",
                    "var b: bool := x.Contains(2); "
                    "if b then Console.WriteLine(\"yes\"); end; "
                    "return x.Count;"),
        "yes\n3\n");
}

// ROW 7 — the INHERITED case: stdlib §4.1's `IList<T>` is the base of a
// USER interface, so the member is INHERITED by it. §14.2 reaches it
// through the declaration-less base refs.
BEZEL_TEST(constraint_collection_member_7086,
           inherited_ilist_member_is_admitted_spec_4_1) {
    expectStdout(
        genericOver("MyI", "x.Add(7); return x.Count;",
                    "public interface MyI: IList<int> end MyI; "),
        "4\n");
}

// ROW 8 — stdlib §4.1: `IList<T>` declares `IsReadOnly` (inherited from
// `ICollection<T>`); §4.1 fixes its value to `false` for every standard
// collection implementing `ICollection<T>`.
BEZEL_TEST(constraint_collection_member_7086,
           icolllection_isread_only_read_is_admitted_spec_4_1) {
    expectStdout(
        genericOver("ICollection<int>",
                    "var r: bool := x.IsReadOnly; "
                    "if r then Console.WriteLine(\"ro\"); "
                    "else Console.WriteLine(\"rw\"); end; "
                    "return 0;"),
        "rw\n0\n");
}

// ROW 9 — stdlib §4.1: `ICollection<T>` declares `Clear`, and `IList<T>`
// declares `Remove` (stdlib §4.1) — both admitted, both really applied to
// the collection the parameter stands for.
BEZEL_TEST(constraint_collection_member_7086,
           ilist_clear_call_is_admitted_and_mutates_spec_4_1) {
    expectStdout(genericOver("IList<int>", "x.Clear(); return x.Count;"),
                 "0\n");
}

// ROW 10 — stdlib §4.1: `ICollection<T>` declares `Remove(item: T): bool`.
// The call is admitted AND typed `bool` — the answer is the member's own.
BEZEL_TEST(constraint_collection_member_7086,
           icolllection_remove_call_is_admitted_spec_4_1) {
    expectStdout(
        genericOver("ICollection<int>",
                    "var r: bool := x.Remove(2); "
                    "if r then Console.WriteLine(\"removed\"); "
                    "else Console.WriteLine(\"absent\"); end; "
                    "return x.Count;"),
        "removed\n2\n");
}

// ROW 11 — stdlib §4.12: `IEnumerable<T>` declares `GetEnumerator(): owned
// IEnumerator<T>`. The call is admitted and the enumerator really iterates
// the collection the parameter stands for — three elements, counted.
BEZEL_TEST(constraint_collection_member_7086,
           enumerable_getenumerator_call_is_admitted_spec_4_12) {
    expectStdout(
        genericOver("IEnumerable<int>",
                    "using var e := x.GetEnumerator(); "
                    "var n: int := 0; "
                    "while e.MoveNext() do n := n + 1; end; "
                    "return n;"),
        "3\n");
}

// ===========================================================================
// THE CONTROLS — the surface is PER HEAD, so the rejections stay.
// ===========================================================================

// CONTROL 1 — a name NO head declares.
BEZEL_TEST(constraint_collection_member_7086,
           ilist_call_nope_is_still_rejected_spec_4_1) {
    expectRejected(genericOver("IList<int>", "x.Nope(1); return 0;"),
                   "no member `Nope` on bare type parameter `T`");
}

// CONTROL 2 — `IndexOf` is declared on `Array<T>` (stdlib §4.4), on no
// collection interface (§4.1).
BEZEL_TEST(constraint_collection_member_7086,
           ilist_call_indexof_is_still_rejected_spec_4_1) {
    expectRejected(genericOver("IList<int>", "x.IndexOf(2); return 0;"),
                   "no member `IndexOf` on bare type parameter `T`");
}

// CONTROL 3 — `IReadOnlyList<T>` declares NO `Add` (§4.1): the surface is
// per head, so the same name that ROW 1 admits on `IList<T>` is refused
// here.
BEZEL_TEST(constraint_collection_member_7086,
           ireadonlylist_call_add_is_still_rejected_spec_4_1) {
    expectRejected(genericOver("IReadOnlyList<int>", "x.Add(1); return 0;"),
                   "no member `Add` on bare type parameter `T`");
}

// CONTROL 4 — `Insert` is `IList<T>`'s alone (§4.1), not `ICollection<T>`'s.
BEZEL_TEST(constraint_collection_member_7086,
           icolllection_call_insert_is_still_rejected_spec_4_1) {
    expectRejected(
        genericOver("ICollection<int>", "x.Insert(0, 1); return 0;"),
        "no member `Insert` on bare type parameter `T`");
}

// CONTROL 5 — an UNCONSTRAINED type parameter establishes nothing (§3.6
// rule 2 / §14.2), so the same call is still refused.
BEZEL_TEST(constraint_collection_member_7086,
           unconstrained_param_call_add_is_still_rejected_spec_3_6) {
    expectRejected(
        "module M7086; class C public static method G<T>(x: T): int "
        "x.Add(1); return 0; end G; "
        "public static method Main(): int return 0; end Main; end C;",
        "no member `Add` on bare type parameter `T`");
}

// CONTROL 6 — the METHOD-REFERENCE form (§27.6) of a collection member stays
// refused: #7014 ruled that the reference form over a collection head is not
// emitted (its call form is fenced, so the reference form must agree), and
// the reference form over the array-backed list heads is #7014's own
// deferral. The refusal is unchanged by this fix.
BEZEL_TEST(constraint_collection_member_7086,
           icolllection_method_ref_is_still_rejected_spec_27_6) {
    expectRejected(
        genericOver("ICollection<int>",
                    "var g: (int) -> bool := x.Contains; return 0;"),
        "no member `Contains` on bare type parameter `T`");
}

// CONTROL 7 — the OWNING enumerator (stdlib §4.13) stays refused: admitting
// it SEGFAULTs (MEASURED, exit 139), and a program that compiles and then
// crashes is worse than a false rejection.
BEZEL_TEST(constraint_collection_member_7086,
           owning_enumerable_getowning_enumerator_is_still_rejected_spec_4_13) {
    expectRejected(
        genericOver("IOwningEnumerable<int>",
                    "using var e := x.GetOwningEnumerator(); return 0;"),
        "no member `GetOwningEnumerator` on bare type parameter `T`");
}

// CONTROL 8 — the enumeration call through a type parameter whose
// substitution is a LIST INTERFACE is REJECTED, not crashed on: the
// collection-interface heads erase the concrete backing, so there is no
// enumerator to build (the non-generic spelling of the same shape is
// rejected the same way — #2457). MEASURED without this refusal: exit 139.
BEZEL_TEST(constraint_collection_member_7086,
           enumerable_through_list_interface_is_rejected_spec_4_12) {
    expectRejected(
        "module M7086; class C public static method G<T>(x: T): int "
        "where T: IEnumerable<int> "
        "using var e := x.GetEnumerator(); "
        "var n: int := 0; "
        "while e.MoveNext() do n := n + 1; end; "
        "return n; end G; "
        "public static method Main(): int "
        "var a: Array<int> := [1, 2, 3]; "
        "var il: IList<int> := a; "
        "Console.WriteLine(G(il)); "
        "return 0; end Main; end C;",
        "`GetEnumerator` through a type parameter has no dispatch");
}

// CONTROL 9 — a USER-CLASS substitution: the compiler can call a collection
// member through a constrained type parameter only for an array-backed
// collection. The class claims `IList<int>` (which the compiler accepts — the
// builtin heads carry no member table to check the claim against, unlike
// §12.5's check for a USER interface), and the call has no dispatch: sema
// bound no member symbol (the head carries no declaration) and the collection
// dispatch is structural. Without this refusal the call was DROPPED
// SILENTLY — MEASURED, the user's own `Add` was never called and the program
// printed the wrong answer with NO diagnostic. A loud refusal is the honest
// answer for a shape the compiler cannot lower.
BEZEL_TEST(constraint_collection_member_7086,
           user_class_substitution_is_rejected_spec_4_1) {
    expectRejected(
        "module M7086; public class MyL : IList<int> "
        "public method Add(v: int) Console.WriteLine(\"added\"); end Add; "
        "public method GetEnumerator(): IEnumerator<int> "
        "return new Array<int>(0).GetEnumerator(); end GetEnumerator; "
        "end MyL; "
        "class C public static method G<T>(x: T): int where T: IList<int> "
        "x.Add(7); return 1; end G; "
        "public static method Main(): int "
        "var m: MyL := new MyL(); "
        "Console.WriteLine(G(m)); "
        "return 0; end Main; end C;",
        "`Add` through a type parameter has no dispatch");
}
