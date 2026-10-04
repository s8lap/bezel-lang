// fix #2673 — spec §15.4 "Collection literals" (v0.268), the borrowing-family
// fresh-producer clause, verbatim:
//
//   "For **borrowing-family** targets (`Array<T>`, `Set<T>`, `LinkedList<T>`,
//    `Stack<T>`, `Queue<T>`, and the key and value positions of
//    `Dictionary<K, V>`), each element expression of **non-trivial-dispose
//    class type** (§28.9) must be a **borrow** whose source satisfies the
//    store rule of §28.11; an element expression that is a **fresh producer**
//    of such a type — an unbound owned temporary (§28.3): a `new` expression,
//    a nested collection literal targeting a non-trivial-dispose collection
//    type, or an `owned`-returning call — is rejected at the literal site."
//
// The clause states its own justification: "The temporary would be disposed at
// the end of the declaration statement, leaving the constructed collection
// holding a dangling borrow from birth — the statically-certain dangle of
// §9.7's no-temporary-source rule, and an escape of a fresh owned value into a
// non-owning location (§28.6)."
//
// WHAT THE COMPILER DID BEFORE THIS FIX — measured on `756fc8c`, not inferred.
// The rejection was not implemented AT ALL: every one of the nine reject rows
// below compiled clean and exited 0. Worse than an accepted-but-diagnosed
// program, the accepted code was silently wrong in the way §28.9 makes
// observable — the element's destructor never ran and its `owned` fields were
// never released, so the two shapes on the issue leaked 24 B and 40 B
// respectively. The point of this fix is the LOUD rejection; the leak is the
// symptom that made it visible, and it disappears with the program.
//
// WHAT §15.4 DOES *NOT* REJECT, and why half this file is accept-controls: a
// rejection that is too wide is a worse defect than the one it fixes, and this
// clause has four explicit neighbours it must not touch.
//
//   1. TRIVIAL-DISPOSE elements. §15.4's very next paragraph: "A fresh element
//      of **trivial-dispose** class type (§28.9) is admitted — not as a
//      borrow, and not as an unbound temporary of its own, but as a **fresh
//      node of the literal tree**". `Array<int>`, `Array<string>`, and a fresh
//      `new Box(7)` where `Box` has no destructor and no `owned` field all
//      stay admissible.
//   2. The OWNING family. "or uses the owning family (`OwningArray<Connection>`),
//      which takes ownership" — the clause's own printed remedy. An
//      `OwningArray`/`OwningSet`/`OwningDictionary` literal of fresh producers
//      is exactly the correct program.
//   3. The NAMED-SOURCE remedy. "The remedy names the source first — `using
//      var c := new Connection("a"); var conns: Array<Connection> := [c];`" —
//      a named binding is a borrow, not a fresh producer.
//   4. `Add` AFTER CONSTRUCTION. §15.4 resolution 5, verbatim: "**`Add` after
//      construction is not a literal position** (the store paragraph below).
//      Aggregate membership never grows." The asymmetry is deliberate and
//      recorded; `coll.Add(new Conn(4))` must keep compiling. Widening this
//      fix to ingress would contradict the paragraph it implements.
//
// Every accept-control below is pinned BY VALUE, not by "it compiles" — a
// control that only asserts `r.ran` cannot tell a preserved program from a
// preserved-but-miscompiled one.
//
// SCOPE — DECLINED WITH REASONS. §15.4 names THREE fresh-producer forms; this
// fix implements two (`new` and the `owned`-returning call). The third, "a
// nested collection literal targeting a non-trivial-dispose collection type"
// (`var xs: Array<OwningArray<Conn>> := [[c]];`), needs a non-trivial-dispose
// verdict for a BUILTIN collection head, and no such authority exists today:
// `common::isNonTrivialDisposeClass` takes a `sema::TypeSymbol*`, which every
// builtin collection head lacks. Inventing a second classification there would
// duplicate a predicate rather than reuse one — the recorded dominant defect
// generator in this codebase — so that arm is filed as a follow-up instead.
// Likewise, the rejection is applied at the DECLARATION literal site, which is
// where §15.4 writes its own example and remedy; the other literal positions
// (call argument, field initializer, return) are the same follow-up.
//
// fail-without/pass-with: on `756fc8c` every `BEZEL_EXPECT_FALSE(r.ran)` below
// fails (the program compiles and runs); with the fix each reports the §15.4
// diagnostic and `r.ran` is false. The accept-controls pass identically before
// and after — that is what makes them controls.
// per §15.4, §28.3, §28.9, §28.11 (issue #2673, spec v0.268)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The two declarations every program below shares.
//   `Conn`  — non-trivial-dispose by DESTRUCTOR (§28.9's first disqualifier).
//   `Own`   — non-trivial-dispose by an `owned` FIELD of non-trivial-dispose
//             type, with no destructor of its own (§28.9's second). The issue
//             reported both shapes; they reach the predicate by different arms
//             of the class-chain walk, so both are exercised.
//   `Box`   — TRIVIAL-dispose (no destructor, no `owned` field). The control.
//   `F.Make` — the `owned`-returning call, §15.4's third-named fresh producer
//             form (the one this fix does implement).
const char* kPrelude =
    "module M; "
    "class Conn(id: int) "
    "  public Id: int := id; "
    "  destructor end destructor; "
    "end Conn; "
    "class Box(v: int) "
    "  public V: int := v; "
    "end Box; "
    "class Own "
    "  public owned F: Conn? := null; "
    "end Own; "
    "class F "
    "  public static method Make(): owned Conn "
    "    return new Conn(9); "
    "  end Make; "
    "end F; ";

// Wrap a `Main` body in the prelude + a host class.
std::string prog(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// True iff SOME diagnostic carries §15.4's borrowing-family fresh-producer
// rejection. Matched on the RULE's own words plus the §-citation, so a
// different §15.4 diagnostic (the target-not-on-list rejection, the dict-form
// mismatch, the inline-array arity error) cannot satisfy it.
bool rejectedBorrowingFamilyFreshProducer(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(
                "is a fresh producer of the non-trivial-dispose class type") !=
                std::string::npos &&
            d.message.find("does not take ownership") != std::string::npos &&
            d.message.find("§15.4") != std::string::npos)
            return true;
    return false;
}

// Assert one reject row: the program does not run, and it failed for THIS rule.
void expectRejected(const std::string& body) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowingFamilyFreshProducer(r));
}

// True iff SOME diagnostic offers the OWNING-FAMILY half of §15.4's remedy.
// §15.4 makes ownership transfer apply "uniformly [to] `OwningArray<T>`,
// `OwningLinkedList<T>`, `OwningStack<T>`, `OwningQueue<T>`, `OwningSet<T>`,
// and `OwningDictionary<K, V>` (**value position**)" — so this half is correct
// everywhere EXCEPT a rejected dictionary KEY, where `OwningDictionary`'s key
// position borrows exactly as `Dictionary`'s does. A diagnostic that names a
// remedy which does not work is worse than one that names none.
bool offersOwningFamilyRemedy(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("use the owning family") != std::string::npos)
            return true;
    return false;
}

// Assert one accept row BY VALUE: it compiles, runs, and computes `expected`.
void expectAccepted(const std::string& body, int expected) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

}  // namespace

// =====================================================================
// REJECT — the six borrowing-family heads §15.4 names, `new` element.
// =====================================================================

// The issue's cardinal repro, shape 1. Pre-fix: compiled, exited 0, leaked
// 24 B, and `Conn`'s destructor never ran. // per §15.4, §28.9 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           array_new_element_rejected_spec_15_4) {
    expectRejected("    var xs: Array<Conn> := [new Conn(1)]; return 0; ");
}

// §15.4 names `Set<T>` in the same parenthesis as `Array<T>`; the rule is one
// rule over the whole list, so each head is pinned separately — a fix that
// keyed on the name "Array" alone would pass the row above and fail here (that
// exact narrowing is the shape of #2142's defect in the neighbouring codegen
// pass). // per §15.4 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           set_new_element_rejected_spec_15_4) {
    expectRejected("    var xs: Set<Conn> := [new Conn(1)]; return 0; ");
}

// per §15.4 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           linked_list_new_element_rejected_spec_15_4) {
    expectRejected("    var xs: LinkedList<Conn> := [new Conn(1)]; return 0; ");
}

// per §15.4 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           stack_new_element_rejected_spec_15_4) {
    expectRejected("    var xs: Stack<Conn> := [new Conn(1)]; return 0; ");
}

// per §15.4 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           queue_new_element_rejected_spec_15_4) {
    expectRejected("    var xs: Queue<Conn> := [new Conn(1)]; return 0; ");
}

// §15.4 says "the **key and value positions** of `Dictionary<K, V>`" — BOTH,
// which is why the dictionary contributes two rows and not one. The key
// position is the easier one to miss: a walk over `cl->elements` alone (the
// dict form stores keys in a PARALLEL `cl->keys` vector, ast/Node.hpp) would
// silently admit this program. // per §15.4 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           dictionary_key_position_rejected_spec_15_4) {
    expectRejected(
        "    var d: Dictionary<Conn, int> := [new Conn(1): 5]; return 0; ");
}

// per §15.4 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           dictionary_value_position_rejected_spec_15_4) {
    expectRejected(
        "    var d: Dictionary<int, Conn> := [5: new Conn(1)]; return 0; ");
}

// =====================================================================
// REJECT — the other two axes: how the class earns non-trivial-dispose,
// and the second fresh-producer FORM §15.4 names.
// =====================================================================

// The issue's cardinal repro, shape 2: `Own` declares NO destructor. It is
// non-trivial-dispose solely because it holds an `owned` field of
// non-trivial-dispose type (§28.9: "no destructor, no `owned` fields of
// non-trivial-dispose type"). Pre-fix: compiled, exited 0, leaked 40 B, and
// the `owned` field was never released. This row is what proves the predicate
// consults the §28.9 class-chain fact rather than testing for a destructor.
// per §15.4, §28.9 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           owned_field_class_element_rejected_spec_15_4_28_9) {
    expectRejected("    var xs: Array<Own> := [new Own()]; return 0; ");
}

// §15.4's second-named fresh-producer form: "or an `owned`-returning call".
// `F.Make()` returns `owned Conn`. The dangle is identical to `new`'s — the
// call's result is an unbound owned temporary (§28.3) either way — so the rule
// covers both, and a fix keyed on `NewExpr` alone would leave this open.
// per §15.4, §28.3 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           owned_returning_call_element_rejected_spec_15_4_28_3) {
    expectRejected("    var xs: Array<Conn> := [F.Make()]; return 0; ");
}

// =====================================================================
// The DIAGNOSTIC's remedies must both be correct, per position.
// =====================================================================

// The sequence-shaped heads and the dictionary VALUE position both have an
// owning-family remedy, and §15.4 prints it: "or uses the owning family
// (`OwningArray<Connection>`), which takes ownership".
// per §15.4 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           sequence_rejection_offers_owning_family_remedy_spec_15_4) {
    JitResult r = runAot(prog("    var xs: Array<Conn> := [new Conn(1)]; return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowingFamilyFreshProducer(r));
    BEZEL_EXPECT_TRUE(offersOwningFamilyRemedy(r));
}

// …but a rejected dictionary KEY does NOT. §15.4's owning-family sentence ends
// "and `OwningDictionary<K, V>` (**value position**)" — the owning
// dictionary's KEY position borrows exactly as `Dictionary`'s does, so
// pointing the reader at `OwningDictionary` would name a remedy that does not
// remedy anything. The rejection still fires (row above); only the second half
// of the remedy is withheld. This row is what keeps that distinction from
// being silently dropped by a later edit to the message.
// per §15.4 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           dictionary_key_rejection_withholds_owning_family_remedy_spec_15_4) {
    JitResult r = runAot(
        prog("    var d: Dictionary<Conn, int> := [new Conn(1): 5]; return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowingFamilyFreshProducer(r));
    BEZEL_EXPECT_FALSE(offersOwningFamilyRemedy(r));
}

// =====================================================================
// ACCEPT-CONTROLS — the four neighbours §15.4 explicitly protects.
// Each pinned by VALUE.
// =====================================================================

// §15.4: "A fresh element of **trivial-dispose** class type (§28.9) is
// admitted". A primitive element is the floor case of that. `Count` = 3.
// per §15.4, §28.9 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           trivial_dispose_int_elements_admitted_spec_15_4_28_9) {
    expectAccepted("    var xs: Array<int> := [1, 2, 3]; return xs.Count; ", 3);
}

// `string` is on §28.9's runtime-managed list by name ("`string` — pervasive").
// `Count` = 2. // per §15.4, §28.9 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           trivial_dispose_string_elements_admitted_spec_15_4_28_9) {
    expectAccepted(
        "    var xs: Array<string> := [\"a\", \"b\"]; return xs.Count; ", 2);
}

// THE control that separates this fix from an over-broad one: `Box` is a
// CLASS, and `new Box(7)` is a fresh producer — but `Box` is trivial-dispose
// (no destructor, no `owned` field), so §15.4 admits it "as a **fresh node of
// the literal tree**". A fix that rejected every fresh class producer would
// break this. Read back through the element: 7.
// per §15.4, §28.9 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           trivial_dispose_class_fresh_element_admitted_spec_15_4_28_9) {
    expectAccepted("    var xs: Array<Box> := [new Box(7)]; return xs[0].V; ", 7);
}

// §15.4's OWN printed remedy, first half: "The remedy names the source first —
// `using var c := new Connection(\"a\"); var conns: Array<Connection> := [c];`".
// A diagnostic that names a remedy which does not compile is worse than one
// that names none, so the remedy is executed, not merely accepted: 7.
// per §15.4, §28.11 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           named_source_remedy_compiles_and_reads_back_spec_15_4) {
    expectAccepted(
        "    using var c := new Conn(7); "
        "    var xs: Array<Conn> := [c]; "
        "    return xs[0].Id; ",
        7);
}

// §15.4's OWN printed remedy, second half: "or uses the owning family
// (`OwningArray<Connection>`), which takes ownership". The SAME fresh producer
// that the `Array<Conn>` row above rejects is correct here — which is exactly
// what makes the rejection a target-family rule and not a producer rule. 9.
// per §15.4, §28.6 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           owning_family_remedy_compiles_and_reads_back_spec_15_4) {
    expectAccepted(
        "    using var xs: OwningArray<Conn> := [new Conn(9)]; "
        "    return xs[0].Id; ",
        9);
}

// The owning family is a FAMILY, not one head — `OwningSet` must keep
// compiling for the same reason `Set` must start rejecting. `Count` = 1.
// per §15.4, §28.6 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           owning_set_literal_of_fresh_producers_admitted_spec_15_4) {
    expectAccepted(
        "    using var xs: OwningSet<Conn> := [new Conn(9)]; return xs.Count; ",
        1);
}

// The dictionary's owning twin, value position — the mirror of the two reject
// rows above. `Count` = 1. // per §15.4, §28.6 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           owning_dictionary_value_of_fresh_producer_admitted_spec_15_4) {
    expectAccepted(
        "    using var d: OwningDictionary<int, Conn> := [5: new Conn(9)]; "
        "    return d.Count; ",
        1);
}

// §15.4 resolution 5, VERBATIM: "**`Add` after construction is not a literal
// position** (the store paragraph below). Aggregate membership never grows."
// The asymmetry is the recorded design, so a fix that widened to ingress would
// contradict the very paragraph it implements. `Length` = 1.
// per §15.4 resolution 5, §28.11 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           add_after_construction_is_not_a_literal_position_spec_15_4) {
    expectAccepted(
        "    var xs: Array<Conn> := []; "
        "    xs.Add(new Conn(4)); "
        "    return xs.Count; ",
        1);
}

// §15.4's empty-literal carve-out: "the empty literal `[]` is admitted for
// every collection target listed above". There is no element to classify, so
// the new check must be a no-op — and must not, for instance, fault on the
// empty `elements` vector. `Count` = 0. // per §15.4 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           empty_borrowing_family_literal_admitted_spec_15_4) {
    expectAccepted("    var xs: Array<Conn> := []; return xs.Count; ", 0);
}

// §15.4's literal-tree paragraph relies on this shape staying legal: "in the
// `groups` and `config` examples below, the nested `Array<int>` ... values are
// fresh nodes of their literals' trees, aggregate-owned by the receiving
// bindings". A nested literal at a TRIVIAL-dispose collection target is
// therefore admitted — and it is the neighbour of the declined third
// fresh-producer form (a nested literal at a NON-trivial-dispose collection
// target), so pinning it here marks the boundary the follow-up must not cross
// when it lands. Outer `Count` = 2. // per §15.4 (#2673)
BEZEL_TEST(sema_borrowing_family_literal_fresh_producer_2673,
           nested_trivial_dispose_literal_admitted_spec_15_4) {
    expectAccepted(
        "    var g: Array<Array<int>> := [[1, 2], [3]]; return g.Count; ", 2);
}
