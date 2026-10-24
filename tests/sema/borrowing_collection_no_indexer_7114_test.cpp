// fix #7114 (§23.4 / §4.6 / §4.10 / §4.11 / §4.18, v0.343) — an index
// expression on a stdlib collection that declares NO indexer compiled, ran,
// exited 0, and printed the WRONG answer.
//
// THE RULE. §23.4 (v0.343): "An indexer is a property whose name is replaced by
// a parameter list in square brackets." An indexer is therefore a DECLARED
// member, and an index expression names one. stdlib §4.6 (`LinkedList<T>`),
// §4.10 (`Set<T>`), §4.11 (`Stack<T>` / `Queue<T>`) and §4.18
// (`SortedSet<T>`) declare no indexer — the stdlib's own listings for those
// five types are complete and list no `property [i: ...]`. The borrowing
// LIST INTERFACES are the ones that declare one: stdlib §4.1 declares
// `IReadOnlyList<T>` with `readonly property [i: int]: T` and `IList<T>` with
// `property [i: int]: T`; `ICollection<T>` and `IReadOnlyCollection<T>` do
// NOT. So `x[0]` on a `LinkedList<T>` names nothing — the same rule §2.7's
// name resolution gives for a name no scope holds — and the reference must
// not resolve.
//
// WHAT WENT WRONG. The IndexExpr typing chain in ExprTypecheck_Exprs.cpp
// matches on `TypeRef::Kind::InlineArray`, `Primitive`+`string`, and `Named`
// with a declaration (a user class/record/interface — where the #2800 arm
// already rejects "type `X` does not declare an indexer"). A stdlib
// collection is a `Named` TypeRef with NO declaration, so it reaches the
// built-in-collection branch, whose indexer-bearing heads are
// `Array`/`OwningArray`/`OwningBuffer`/`FrozenArray`/the list interfaces
// (`isArrLike` / `isSpan`) and the dictionaries (`isDict`). The five
// borrowing collections above are in NEITHER list, so the IndexExpr was left
// UNTYPED. Codegen's #2434 backstop then degraded the read, and codegen's
// lowerCollectionIndexerGet picked the receiver up through
// `isCollectionTypeName` (BuiltinRegistry.cpp's `kArrayBackedCollections`,
// which registers Stack/Queue/Set/SortedSet for NON-indexer operations like
// `.Count` — not for a positional read), so the program compiled.
//
// MEASURED on the pre-fix base (r30-c1 tip 9c966ddf), one compile + one run
// per row, for `var v: int := x[0];` after one element was stored:
//   * `LinkedList<int>` — the #2434 backstop degraded the read to a
//     zero-initialised value. The program COMPILED, RAN, exited 0, and
//     printed `0` where the list held `1`. This is the issue's own repro.
//   * `Stack<int>` / `Queue<int>` — NO diagnostic at all. Codegen read the
//     collection's array backing store directly and printed `1`: a
//     plausible-but-wrong answer (a Stack iterates top-to-bottom and a
//     Queue front-to-back, and NEITHER declares a positional read at all).
//   * `Set<int>` / `SortedSet<int>` — NO diagnostic. The hash-table backing
//     store was read as if it were an array and the program printed a
//     garbage value.
// All five are ONE defect in ONE arm, so all five are rejected.
//
// THE FIX extends #5588's rejection arm (the one that already rejects the
// `Owning…` half of each pair — `OwningStack<T>`, `OwningQueue<T>`,
// `OwningLinkedList<T>`, `OwningSet<T>`, `OwningSlotMap<T>`) with the
// borrowing half, in the same branch, with the same diagnostic wording.
//
// TEST MATRIX:
//   T1  the issue's EXACT repro, plus the four sibling types: each is
//       REJECTED at the driver level, with the §23.4 diagnostic naming the
//       type, and demonstrably NOT the #2434 degraded-lowering warning that
//       was the pre-fix symptom, and not a crash / signal / IR failure.
//       Plus a `runAotOut` row pinning that the issue's program — which used
//       to print `0` — no longer runs at all.
//   T2  the five borrowing collections still work for their OTHER operations
//       (Push/Enqueue/AddLast/Add + Count): the rejection must not break
//       working code.
//   T3  ACCEPT-CONTROLS that RUN and assert real values — every indexable
//       receiver family stays admitted: `Array<T>`, `string` (§4.6's char
//       indexer), a user class's OWN indexer, and §23.4's MANDATED `this[i]`
//       spelling inside a class that declares an indexer (#2795's spelling).
//   T4  the OVER-REJECTION guard: a declared generic PARAMETER that happens
//       to be named `Set` is the DECLARATION's own type parameter (§14.2),
//       not the stdlib builtin, and must NOT be rejected by this rule.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot / runAotChecked / runAotOut
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

constexpr std::string_view kSec234 = "\xC2\xA7" "23.4";

std::string prog7114(std::string_view body) {
    return std::string("module M; class C public static method Main(): int ") +
           std::string(body) + " end Main; end C;";
}

// A driver-level rejection row: the whole `bezelc` pipeline must refuse the
// program, name this diagnostic, must NOT emit the #2434 degraded-lowering
// notice that was the pre-fix symptom, and must not crash.
void expectDriverRejectsBorrowingCollectionIndexer(const std::string& body,
                                                    const char* typeSpelling) {
    auto r = runAot(prog7114(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("type `") + typeSpelling +
            "<T>` does not declare an indexer")
               .c_str()));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kSec234).c_str()));
    // NOT the pre-fix symptom: the #2434 internal degraded-lowering notice.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "#2434"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "degraded to a zero-initialized value"));
    // NOT a crash, NOT a codegen failure, NOT a signal or a timeout.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Assertion"));
}

}  // namespace

// ===========================================================================
// T1 — the issue's exact repro and its four siblings are rejected.
// ===========================================================================
BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t1_issue_exact_linked_list_rejected_spec_23_4) {
    // Pre-fix: exit 0, the #2434 warning, and the program PRINTED `0`.
    expectDriverRejectsBorrowingCollectionIndexer(
        " var ll := new LinkedList<int>(); "
        " _ := ll.AddLast(1); "
        " var v: int := ll[0]; "
        " Console.WriteLine(v); return 0; ",
        "LinkedList");
}

BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t1_stack_index_rejected_spec_23_4) {
    // Pre-fix: exit 0, NO diagnostic, and the program printed a
    // plausible-but-wrong `1` read out of the backing store.
    expectDriverRejectsBorrowingCollectionIndexer(
        " var s := new Stack<int>(); "
        " s.Push(1); "
        " var v: int := s[0]; "
        " Console.WriteLine(v); return 0; ",
        "Stack");
}

BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t1_queue_index_rejected_spec_23_4) {
    expectDriverRejectsBorrowingCollectionIndexer(
        " var q := new Queue<int>(); "
        " q.Enqueue(1); "
        " var v: int := q[0]; "
        " Console.WriteLine(v); return 0; ",
        "Queue");
}

BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t1_set_index_rejected_spec_23_4) {
    // Pre-fix: exit 0, NO diagnostic, and the program printed a garbage
    // value read out of the hash table's backing store.
    expectDriverRejectsBorrowingCollectionIndexer(
        " var s := new Set<int>(); "
        " s.Add(1); "
        " var v: int := s[0]; "
        " Console.WriteLine(v); return 0; ",
        "Set");
}

BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t1_sorted_set_index_rejected_spec_23_4) {
    expectDriverRejectsBorrowingCollectionIndexer(
        " var s := new SortedSet<int>(); "
        " s.Add(1); "
        " var v: int := s[0]; "
        " Console.WriteLine(v); return 0; ",
        "SortedSet");
}

// The issue's EXACT program. Pre-fix it compiled, ran, exited 0 and printed
// `0` where the list held `1`. runAotOut's "<did-not-run>" sentinel proves
// both halves at once — the program is now REJECTED, so it categorically
// cannot print `0` anymore.
BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t1_issue_exact_program_no_longer_runs_spec_23_4) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "public class Program "
            "  public static method Main(): int "
            "    var ll := new LinkedList<int>(); "
            "    ll.AddLast(1); "
            "    var v: int := ll[0]; "
            "    Console.WriteLine(v); "
            "    return 0; "
            "  end Main; "
            "end Program;"),
        std::string("<did-not-run>"));
}

// ===========================================================================
// T2 — the borrowing collections still work for their OTHER operations.
// A rejection that broke working code would be worse than the bug.
// ===========================================================================
BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t2_linked_list_add_last_and_count_still_work_stdlib_4_6) {
    auto r = runAotChecked(prog7114(
        " var l := new LinkedList<int>(); "
        " _ := l.AddLast(7); "
        " return l.Count; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t2_stack_push_and_count_still_work_stdlib_4_11) {
    auto r = runAotChecked(prog7114(
        " var s := new Stack<int>(); "
        " s.Push(7); "
        " return s.Count; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t2_queue_enqueue_and_count_still_work_stdlib_4_11) {
    auto r = runAotChecked(prog7114(
        " var q := new Queue<int>(); "
        " q.Enqueue(7); "
        " return q.Count; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t2_set_add_and_count_still_work_stdlib_4_10) {
    auto r = runAotChecked(prog7114(
        " var s := new Set<int>(); "
        " s.Add(7); "
        " return s.Count; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t2_sorted_set_add_and_count_still_work_stdlib_4_18) {
    auto r = runAotChecked(prog7114(
        " var s := new SortedSet<int>(); "
        " s.Add(7); "
        " return s.Count; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// T3 — ACCEPT-CONTROLS. Every indexable receiver family stays admitted, each
// RUNS and asserts a real value.
// ===========================================================================
BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t3_array_index_still_works_stdlib_4_4) {
    auto r = runAotChecked(prog7114(
        " var arr: Array<int> := [10, 20, 30]; "
        " return arr[1]; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t3_string_index_still_works_stdlib_4_6_char_indexer) {
    auto r = runAotChecked(prog7114(
        " var s: string := \"hello\"; "
        " Console.WriteLine(s[1].ToString()); return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("e") != std::string::npos);
}

// A user class's OWN indexer (§23.4) — the receiver declares a declaration,
// so it takes the user-defined path, not the built-in-collection path.
BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t3_user_indexer_still_works_spec_23_4) {
    const std::string src =
        "module M; "
        "class Box7114 "
        "  private _v: int := 10; "
        "  public property [i: int]: int => _v + i; "
        "end Box7114; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box7114(); "
        "    return b[1]; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// §23.4's MANDATED spelling: "Inside a member of a class with its own
// indexer, `this[i]` is required (the brackets need a left-hand side)" — the
// #2795 spelling. It must keep compiling AND reading the element.
BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t3_this_indexer_still_works_spec_23_4) {
    const std::string src =
        "module M; "
        "class Holder7114 "
        "  private _v: int := 7; "
        "  public property [i: int]: int "
        "    get => _v + i; set => do _v := value - i; end; end; "
        "  public method Read(): int return this[0]; end Read; "
        "  public method Write() this[0] := 42; end Write; "
        "end Holder7114; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder7114(); "
        "    h.Write(); "
        "    return h[0]; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// T4 — the OVER-REJECTION guard. A declared generic PARAMETER named `Set` is
// the DECLARATION's own type parameter (§14.2), NOT the stdlib builtin, and
// must not be rejected by this rule. The constraint gives the parameter an
// indexer of its own (#794's shape), so the access resolves and reads the
// element: 40 + 2 = 42.
// ===========================================================================
BEZEL_TEST(sema_borrowing_collection_no_indexer_7114,
           t4_type_parameter_named_set_is_not_the_builtin_spec_14_2) {
    const std::string src =
        "module M; "
        "interface IIndexable readonly property [i: int]: int; end IIndexable; "
        "class SingleInt: IIndexable "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int => _v + i; "
        "end SingleInt; "
        "class C "
        "  public static method At<Set>(x: Set, i: int): int "
        "    where Set: IIndexable "
        "    return x[i]; "
        "  end At; "
        "  public static method Main(): int "
        "    using var s := new SingleInt(40); "
        "    return At<SingleInt>(s, 2); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
