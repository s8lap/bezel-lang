// fix #8283 / #8329 — a user `extension Object` member reaches a LAZY-SEQUENCE
// receiver (§26.5 rule 2), and a stdlib §5 `where` clause is an APPLICABILITY
// filter, not a ranking signal (§26.5.1, v0.355).
//
// TWO ISSUES, ONE MECHANISM.
//
// #8283 (§26.5): a NON-§5-named user `extension Object` member called on a
// lazy-sequence receiver was accepted and the statement silently dropped —
// then, after #8310's gate landed, REJECTED with "no member `Frob` on
// `IEnumerable` (stdlib §4.12)".
//
// THE DEFECT (#8283). stdlib §5's deferred operators produce a "lazy
// enumerable": a Named TypeRef with no declaration, no astNode and no
// importedName, carrying the element type at genericArgs[0]
// (`makeLazyEnumerable`, src/sema/ExprTypecheck_Calls.cpp). The sentinel
// carries NO NAME, so §26.5 rule 2's search ("the compiler searches the
// current module AND all imported modules for extension blocks on `x`'s type,
// base types, or implemented interfaces") related it to NO extension block at
// all — not to stdlib §5's own `IEnumerable<T>` block, and not to the implicit
// `Object` base ("every type derives from Object", §2 / §12).
//
// The `Object` base IS a base type of the sentinel's type (§2 / §12), and
// rule 2 searches the base types — so a user's `extension Object` member is a
// candidate for EVERY name the sentinel does not commit. The #8179 block that
// gave the sentinel that base was SCOPED to a §5-named member whose arity
// matched no §5 overload, so a name that is no §5 operator at all (`Frob`,
// `Describe`) never reached it.
//
// THE DEFECT (#8329). stdlib §5.1's no-selector `Min()` carries the block
// level `where T: IComparable<T>`, and at an `Array<int>` element that clause
// is PROVABLY unsatisfied (spec §14.2 rule 1: "T is a class that lists I (or
// any interface extending I) in its inheritance clause" — a builtin class
// lists no interface at all). §26.5.1 says a block-level `where` clause is an
// APPLICABILITY FILTER: "extension members of the called name that match the
// call's arity and argument types, from blocks whose head matches R (step 2 of
// §26.5), with block-level `where` clauses satisfied by the arguments
// inferred from the receiver. A `where` clause is an *applicability* filter,
// never a ranking signal." So stdlib §5's `Min()` is NOT a candidate at such
// an element, and the user's own `extension Object { Min() }` must answer.
//
// The #8179 block asked the WRONG question — "does a §5 overload EXIST at this
// arity?" — where §26.5.1 asks "is a §5 overload APPLICABLE at this
// receiver?". And on a NAMED `IEnumerable<Array<int>>` receiver the sentinel's
// base does not apply at all: the receiver is a real (named) `IEnumerable<T>`,
// and the collection-INTERFACE heads are excluded from
// `builtinHeadReachesObjectExtension` by #7822 ("their `Object` view reads
// freed or foreign memory"), so the §26.5 rule-2 search found no candidate
// at all and the #2770 codegen guard refused the call.
//
// WHERE THE FIX IS. Two places, one per issue:
//   * `src/sema/ExprTypecheck_Calls.cpp`, the #8179 block inside
//     `ExprTC::resolveMemberAccess`: the NAME gate is gone (the sentinel's
//     implicit `Object` base answers every name the sentinel does not
//     commit) and the ARITY gate becomes an APPLICABILITY gate.
//   * `src/sema/ExprTypecheck.cpp`, `ExprTC::lookupExtensionMember`: the
//     `Object` base of a builtin COLLECTION-INTERFACE receiver is admitted
//     ONLY where stdlib §5's own candidate for the called name is
//     INAPPLICABLE at that receiver.
//
// THE CONTROLS below pin that the fix does not OVER-reach. The #7822
// exclusion survives INTACT: a `extension Object` member of a name stdlib §5
// answers is still unreachable on a named `IEnumerable<int>` receiver (row
// R5), and a `Dictionary<K,V>` receiver still refuses it (row R11). Only the
// case stdlib §5 itself cannot answer — and only for a name stdlib §5
// DECLARES — opens the `Object` base.
//
// THE ORACLE. `runAot` / `runAotChecked` — the strict AOT path (fork-isolated
// `Driver::compileFile` + clang link + native exec). Every row asserts the
// program's STDOUT and its exit code: the program's BEHAVIOUR, not "it
// compiled".
//
// SPEC BASIS. §26.5 rule 2, §26.5.1 *Specificity ordering* (v0.355), §2 / §12
// (the implicit `Object` base), §8.1 (the universal `Object` members),
// §14.2 rule 1 (*Interface conformance*), stdlib §4.12 (`IEnumerable<T>`'s
// documented surface), stdlib §5 / §5.1 (`System.Collections.Linq`).

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The #8310 gate's "no member" wording (stdlib §4.12's `IEnumerable<T>`
// surface) and the #2770 codegen guard's `IComparable<T>` refusal.
const char* const kNoMemberOnIEnumerable = "no member `Frob` on `IEnumerable`";
const char* const kNoMemberOnDictionary = "no member `Frob` on `Dictionary`";
const char* const kNotComparable = "requires the element type to satisfy";

// The diagnostic engine has no substring query (only `hasErrors`), so the
// rows that pin a REJECTION read the messages themselves.
bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// ROW R1 — #8283's OWN probe, verbatim: a NON-§5-named `extension Object`
// member on a lazy-sequence receiver. The member must be REACHED, and the
// program must print the member's value (9) — not lose the statement.
std::string lazyReceiverNonLinqName() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Frob(): int => 9;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    Console.WriteLine(a.Where(lambda(x) => x > 0).Frob());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R2 — #8283's SECOND probe shape: a lazy LOCAL receiver and a
// non-§5-named `extension Object` member.
//
// The issue's own spelling of this probe has the member's body call
// `this.ToString()`, whose value on a lazy receiver is #8282's OWN defect
// (the lazy value's word 0 is the lazy vtable, not a class vtable), so this
// row uses a body that does not read `this` — the row pins the #8283
// mechanism (the member is REACHED on a lazy LOCAL receiver), not #8282's
// rendering. The garbage-string interaction is reported, not pinned.
std::string lazyLocalReceiverNonLinqName() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Describe(): int => 9;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    var q := a.Where(lambda(x) => x > 0);\n"
        "    Console.WriteLine(q.Describe());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R3 — #8329's OWN probe (half A): a lazy chain over `Array<Array<int>>`
// and a user's `extension Object { Min() }`. stdlib §5.1's `Min()` carries
// `where T: IComparable<T>`, which an `Array<int>` element provably fails
// (§14.2 rule 1), so §5 is not an applicable candidate and the user's member
// answers (4242).
std::string lazyChainObjectMin() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Min(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<Array<int>>();\n"
        "    Console.WriteLine(a.Where(lambda(x: Array<int>) => true).Min());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R4 — #8329's OWN probe (half B): a NAMED `IEnumerable<Array<int>>`
// receiver. The same §26.5.1 filter applies, and the `Object` base of the
// collection-interface receiver is what answers.
std::string namedEnumerableObjectMin() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Min(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<Array<int>>();\n"
        "    var e: IEnumerable<Array<int>> := a;\n"
        "    Console.WriteLine(e.Min());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R5 — THE HARD CONTROL (the #7822 exclusion). A named `IEnumerable<int>`
// receiver and a `extension Object` member of a name stdlib §5 does NOT
// declare: the member stays unreachable, and the refusal keeps its wording.
// The `Object` view of a collection-interface receiver reads freed or foreign
// memory (#7822), so the exclusion is NOT relaxed for such a name.
std::string namedEnumerableObjectNonLinqName() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Frob(): int => 9;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<int>();\n"
        "    var e: IEnumerable<int> := a;\n"
        "    Console.WriteLine(e.Frob());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R6 — CONTROL. The EAGER `Array<int>` receiver: the same
// `extension Object { Frob() }` already answered there before this fix
// (`builtinHeadReachesObjectExtension`, fix #7775) and must keep answering.
std::string arrayReceiverObjectNonLinqName() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Frob(): int => 9;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<int>();\n"
        "    Console.WriteLine(a.Frob());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R7 — CONTROL. The `IEnumerable<int>`-HEADED extension on the same named
// `IEnumerable<int>` receiver: the head matches the receiver's own head, so
// it answers (9) — the #8283 fix must not disturb it.
std::string namedEnumerableHeadExtension() {
    return
        "module M;\n"
        "extension IEnumerable<int>\n"
        "  public method Frob(): int => 9;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<int>();\n"
        "    var e: IEnumerable<int> := a;\n"
        "    Console.WriteLine(e.Frob());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R8 — CONTROL (the #8199 parent, and the NO-EXTENSION control). With NO
// user extension in scope the no-selector `Min()` over an `Array<int>` element
// is still REFUSED: spec §14.2 rule 1 — a builtin class lists no interface,
// so `Array<int>` does not satisfy `T: IComparable<T>`, and nothing else
// answers the call. A fix that accepted this would be too wide.
std::string arrayOfArrayNoExtension() {
    return
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<Array<int>>();\n"
        "    Console.WriteLine(a.Min());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R9 — CONTROL (the #8199 fix). The same `extension Object { Min() }` on
// the CONCRETE `Array<Array<int>>` receiver still answers (4242) — the
// #8199 fix (9cefd62c) must not regress.
std::string arrayOfArrayObjectMin() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Min(): int => 4242;\n"
        "  public method Max(): int => 4343;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<Array<int>>();\n"
        "    Console.WriteLine(a.Min());\n"
        "    Console.WriteLine(a.Max());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R10 — CONTROL (the §26.5.1 specificity order). A §5-named call at a §5
// arity still takes the §5 operator: stdlib §5's `IEnumerable<T>` head is
// more specific than `Object`, and its overload IS applicable at an `int`
// element, so the user's `extension Object { Min() }` must not pre-empt it.
// The program prints the §5 pipeline's element count (2).
std::string lazyReceiverSameArityStillTakesStdlib() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Select(f: (int) -> int): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(1);\n"
        "    a.Add(2);\n"
        "    var q := a.Where(lambda(x) => x > 0).Select(lambda(x) => x + 1);\n"
        "    Console.WriteLine(q.Count());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// ROW R11 — CONTROL (the dictionary half of #8179, NOT fixed). A
// `Dictionary<string,int>` receiver refuses a `extension Object` member of a
// name stdlib §5 does not declare: the dictionary's `Object` view reads freed
// memory (#7822), so admitting the dictionary family would turn today's
// compile error into a run-time fault.
std::string dictionaryReceiverObjectNonLinqName() {
    return
        "module M;\n"
        "extension Object\n"
        "  public method Frob(): int => 9;\n"
        "end;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var d := new Dictionary<string, int>();\n"
        "    d.Add(\"a\", 1);\n"
        "    Console.WriteLine(d.Frob());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// R1 — #8283. The issue's own probe, verbatim: a NON-§5-named user
// `extension Object` member on a lazy-sequence receiver is REACHED and the
// program prints its value (9).
//
// BASE (r30-c1 @ 3474b850): a COMPILE ERROR —
// "no member `Frob` on `IEnumerable` (stdlib §4.12) -- it is not part of this
//  interface's documented surface (GetEnumerator), a universal
//  `ToString`/`Equals`/`GetHashCode`/`GetType` member (language spec §8.1), or
//  a stdlib §5 LINQ extension operator".
//
// `runAot`, not `runAotChecked`: the lazy pipeline is handed to the extension
// as an OWNED receiver and is never materialised, so it leaks — the
// PRE-EXISTING "an unconsumed lazy pipeline leaks" class (the #8179 pin file
// reports the same for its own row), not a defect of this fix. The row pins
// the VALUE (9), and the leak is reported rather than pinned.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           lazy_receiver_non_linq_name_reaches_the_user_extension) {
    JitResult r = runAot(lazyReceiverNonLinqName());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}

// ---------------------------------------------------------------------------
// R2 — #8283's SECOND shape: a lazy LOCAL receiver and a non-§5-named
// `extension Object` member. The member is REACHED (the statement is not
// lost) and the program prints its value (9).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           lazy_local_receiver_non_linq_name_reaches_the_user_extension) {
    JitResult r = runAot(lazyLocalReceiverNonLinqName());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}

// ---------------------------------------------------------------------------
// R3 — #8329 (half A). A lazy chain over `Array<Array<int>>` and a user's
// `extension Object { Min() }`: stdlib §5.1's `Min()` is NOT applicable at an
// `Array<int>` element (§26.5.1's filter, §14.2 rule 1), so the user's member
// answers (4242).
//
// BASE: a COMPILE ERROR — the #2770 codegen guard ("`Array` does not — its
// `CompareTo` could not be resolved for dispatch").
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           lazy_chain_object_min_answers_the_user_extension) {
    JitResult r = runAot(lazyChainObjectMin());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// R4 — #8329 (half B). A NAMED `IEnumerable<Array<int>>` receiver and the
// same user extension: the collection-interface receiver's `Object` base
// answers where stdlib §5's own candidate is inapplicable.
//
// BASE: a COMPILE ERROR — the same #2770 refusal.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           named_enumerable_object_min_answers_the_user_extension) {
    JitResult r = runAotChecked(namedEnumerableObjectMin());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}

// ---------------------------------------------------------------------------
// R5 — THE HARD CONTROL. A named `IEnumerable<int>` receiver still REFUSES a
// `extension Object` member of a name stdlib §5 does not declare: the
// #7822 exclusion (the collection-interface heads' `Object` view reads freed
// or foreign memory) survives the fix intact.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           named_enumerable_still_refuses_a_non_linq_object_extension) {
    JitResult r = runAot(namedEnumerableObjectNonLinqName());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kNoMemberOnIEnumerable));
    BEZEL_EXPECT_TRUE(hasError(r, "stdlib §4.12"));
}

// ---------------------------------------------------------------------------
// R6 — CONTROL. The EAGER `Array<int>` receiver: the same extension already
// answered there and still does.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           array_receiver_object_non_linq_name_is_unchanged) {
    JitResult r = runAotChecked(arrayReceiverObjectNonLinqName());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}

// ---------------------------------------------------------------------------
// R7 — CONTROL. The `IEnumerable<int>`-HEADED extension on the named
// `IEnumerable<int>` receiver still answers (9).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           named_enumerable_head_extension_is_unchanged) {
    JitResult r = runAotChecked(namedEnumerableHeadExtension());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "9\n");
}

// ---------------------------------------------------------------------------
// R8 — CONTROL (the NO-EXTENSION control). With no user extension in scope
// the no-selector `Min()` over an `Array<int>` element is still REFUSED with
// the §5.1 / #2770 wording: spec §14.2 rule 1 — a builtin class lists no
// interface, so `Array<int>` does not satisfy `T: IComparable<T>`, and
// without a user extension NOTHING answers the call.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           array_of_array_without_an_extension_is_still_refused) {
    JitResult r = runAot(arrayOfArrayNoExtension());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kNotComparable));
    BEZEL_EXPECT_TRUE(hasError(r, "IComparable<T>"));
}

// ---------------------------------------------------------------------------
// R9 — CONTROL (the #8199 fix). The same extension on the CONCRETE
// `Array<Array<int>>` receiver still answers (4242 / 4343).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           array_of_array_object_min_is_unchanged) {
    JitResult r = runAotChecked(arrayOfArrayObjectMin());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n4343\n");
}

// ---------------------------------------------------------------------------
// R10 — CONTROL (the §26.5.1 specificity order). A §5-named call at a §5
// arity still takes the §5 operator: stdlib §5's `IEnumerable<T>` head is
// more specific than `Object` and its overload IS applicable at an `int`
// element, so the user's `extension Object { Select(f) }` must not pre-empt
// it. The program prints the §5 pipeline's element count (2).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           same_arity_call_still_takes_the_stdlib_operator) {
    JitResult r = runAotChecked(lazyReceiverSameArityStillTakesStdlib());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// ---------------------------------------------------------------------------
// R11 — CONTROL (the dictionary half of #8179, NOT fixed). A
// `Dictionary<string,int>` receiver still refuses a `extension Object` member
// of a name stdlib §5 does not declare — the dictionary's `Object` view reads
// freed memory (#7822).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_lazy_ext_object_member_8283_8329,
           dictionary_receiver_still_refuses_a_non_linq_object_extension) {
    JitResult r = runAot(dictionaryReceiverObjectNonLinqName());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kNoMemberOnDictionary));
    BEZEL_EXPECT_TRUE(hasError(r, "stdlib §4.8"));
}
