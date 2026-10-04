// fix #3307 — spec §15.4 "Collection literals" (v0.268), the borrowing-family
// fresh-producer clause, the THIRD fresh-producer form, verbatim:
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
// The first two forms (`new` and the `owned`-returning call) were rejected
// by the #2673 lending fix (`borrowing_family_literal_fresh_producer_2673`).
// This file is the residual: the THIRD form, "a nested collection literal
// targeting a non-trivial-dispose collection type". The shape is
// `var xs: Array<OwningArray<Conn>> := [[...]];` — the outer literal is a
// borrowing-family target, the element is itself a literal, and the element
// literal's target type is an OWNING family head whose element is a
// non-trivial-dispose class — so the element is a fresh owned temporary
// holding non-trivial-dispose storage, and the outer literal's slot for
// it is a borrow.
//
// WHY THIS IS A DIFFERENT DEFECT FROM #2673. The two earlier fresh-producer
// forms are decided by `exprIsOwnedProducerOfNonTrivialDisposeClass`: a
// `new T(...)` or a `calleeReturnsOwned` call whose static result type is a
// user class `common::isNonTrivialDisposeClass` says is non-trivial. That
// predicate takes a `sema::TypeSymbol*` — every BUILTIN collection head
// lacks one (the type is a named, declaration-less TypeRef), so the
// predicate returns false for an `OwningArray<Conn>` element even though
// `#2673` is the right authority for the user-class half of the question.
// The third form needs a verdict for the BUILTIN case, and that verdict is
// a separate predicate (`isSec154BuiltinCollectionNonTrivialDispose`) that
// delegates to the same `isNonTrivialDisposeClass` for the user-class
// element half — the single source of truth the #1414 / #2666 "duplicated
// classification predicates are the dominant defect generator" discipline
// requires.
//
// THE BOUNDARY #3307 DOES NOT CROSS. §15.4 names the third form as
// "a nested collection literal targeting a non-trivial-dispose collection
// type". A borrowing-family target — `Array<Array<Conn>>` — is trivial-
// dispose regardless of its element (borrowing slots do not own storage),
// so the inner literal there is NOT a §15.4 fresh-producer element. The
// pre-existing #2673 test `nested_trivial_dispose_literal_admitted_spec_15_4`
// reads `Array<Array<int>> := [[1, 2], [3]]` and pins that boundary — this
// file lives next to it, not in place of it. Likewise, a nesting at an
// OWNING-family target (`OwningArray<...>`) is the owning family, not
// the borrowing family, and the check is a no-op there (the owning family
// takes ownership). The new arm fires only at the borrowing-family call
// site, for an element that is itself a literal whose target is an owning
// head with non-trivial-dispose class element. The five rows below pin
// exactly that predicate.
//
// WHERE §15.4's REMEDIES APPEAR. The first half — "name the source first" —
// applies everywhere (every accepted rejection is one a fresh binding fixes).
// The second half — "use the owning family" — applies only on the
// sequence/element position and the dict value position, never on the
// dict key position (the spec's "uniformly" sentence ends "and
// `OwningDictionary<K, V>` (VALUE position)"; the key position borrows
// exactly like `Dictionary`'s and naming `OwningDictionary` there would
// send the reader to a non-remedy). The dictionary-with-nested-literal
// key row pins that asymmetry on the new arm.
//
// fail-without/pass-with: on `0049717` (the commit #3307 is fixed against),
// every `BEZEL_EXPECT_FALSE(r.ran)` row below fails — the program compiles
// and runs (§15.4's third fresh-producer form was implemented nowhere).
// With the fix each reports the §15.4 diagnostic and `r.ran` is false.
// The accept-controls pass identically before and after — that is what
// makes them controls.
// per §15.4, §28.3, §28.9, §28.11 (issue #3307, spec v0.268)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The single declaration every program here shares:
//   `Conn` — non-trivial-dispose by DESTRUCTOR (§28.9's first disqualifier).
//            A non-trivial-dispose class type is the only thing that makes
//            `OwningArray<Conn>` non-trivial-dispose by way of its implicit
//            owned element slot.
//   `Box`  — TRIVIAL-dispose (no destructor, no `owned` field). Used as the
//            control element class so accepting `OwningArray<Box>` and
//            rejecting `OwningArray<Conn>` are pinned separately.
const char* kPrelude =
    "module M; "
    "class Conn(id: int) "
    "  public Id: int := id; "
    "  destructor end destructor; "
    "end Conn; "
    "class Box(v: int) "
    "  public V: int := v; "
    "end Box; ";

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
// rejection. The match is on the RULE's own words plus the §-citation, so a
// different §15.4 diagnostic (the target-not-on-list rejection, the dict-form
// mismatch, the inline-array arity error) cannot satisfy it. The match
// also tolerates the body's element type name — the diagnostic now uses
// the element's static type's headName + generic args (e.g.
// `OwningArray<Conn>` for a nested literal at that target), so the
// `is a fresh producer` substring is the stable anchor.
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

// Assert one reject row: the program does not run, and it failed for THIS rule.
void expectRejected(const std::string& body) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowingFamilyFreshProducer(r));
}

// Assert one accept row BY VALUE: it compiles, runs, and computes `expected`.
void expectAccepted(const std::string& body, int expected) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

}  // namespace

// =====================================================================
// REJECT — the third fresh-producer form §15.4 names.
// =====================================================================

// The shape the issue reported as residual: a nested literal at a borrowing-
// family target whose element is itself a literal at a non-trivial-dispose
// collection target. The inner `[new Conn(1)]` is rejected at the OUTER
// literal site (the borrowing-family target) — §15.4 reads rejection at the
// literal site, and the outer literal is the one whose slot is the borrow.
// Per the spec scope, the inner literal's check would also fire if it were
// a declaration literal (and it does, transitively: `new Conn(1)` is one of
// the two #2673 fresh-producer forms); the new arm is what catches the
// SHAPE of the outer literal. Pre-fix: compiled clean (the predicate this
// turns on is not consulted); post-fix: this row reports the §15.4
// diagnostic. // per §15.4 (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           array_owning_array_new_inner_rejected_spec_15_4) {
    expectRejected(
        "    var xs: Array<OwningArray<Conn>> := [[new Conn(1)]]; return 0; ");
}

// The diagnostic's OUTER-TARGET spelling: the offending element is an
// identifier (a borrowed `c`), not a `new`; the inner literal is
// `[c]` at `OwningArray<Conn>` (owning family, non-trivial-dispose). The
// OUTER literal at `Array<OwningArray<Conn>>` is the borrowing-family
// target whose slot is the borrow; the rejection IS the third §15.4 form
// (a nested literal at a non-trivial-dispose collection target). Pre-fix:
// compiles (the predicate doesn't fire); post-fix: rejected. // per §15.4
// (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           array_owning_array_borrowed_inner_rejected_spec_15_4) {
    expectRejected(
        "    using var c := new Conn(1); "
        "    var xs: Array<OwningArray<Conn>> := [[c]]; "
        "    return 0; ");
}

// The dictionary arm: the value position. A nested literal at the value
// position of a borrowing-family dictionary is rejected by §15.4 exactly
// as the sequence-form value position is. The diagnostic is identical
// (a nested literal at a non-trivial-dispose collection target) and the
// owning-family remedy is offered because this is the value position.
// per §15.4 (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           dictionary_value_nested_owning_array_rejected_spec_15_4) {
    expectRejected(
        "    var d: Dictionary<int, OwningArray<Conn>> := [1: [new Conn(1)]]; "
        "    return 0; ");
}

// The dictionary KEY position with the same shape. The check fires
// (the inner literal at `OwningArray<Conn>` is a fresh producer of a
// non-trivial-dispose collection type, regardless of which position
// it occupies), but the owning-family remedy is WITHHELD — §15.4 makes
// ownership transfer apply "uniformly [to] `OwningArray<...>`, ... and
// `OwningDictionary<K, V>` (VALUE position)" — so `OwningDictionary`'s
// KEY position borrows exactly as `Dictionary`'s does, and naming
// `OwningDictionary` as a remedy for a rejected key would send the
// reader to a non-remedy. This row pins that asymmetry on the new arm.
// per §15.4 (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           dictionary_key_nested_owning_array_rejected_owning_remedy_withheld_15_4) {
    JitResult r = runAot(prog(
        "    var d: Dictionary<OwningArray<Conn>, int> := [[new Conn(1)]: 5]; "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowingFamilyFreshProducer(r));
    BEZEL_EXPECT_FALSE(offersOwningFamilyRemedy(r));
}

// The other owning heads. `OwningSet<Conn>` is also non-trivial-dispose
// (owning family, class element). `Set<OwningSet<Conn>>` is the borrowing
// analogue; the inner literal at `OwningSet<Conn>` is the third fresh-
// producer form. A fix keyed on the head name "Array" alone would pass
// the rows above and fail this one — the same shape of #2142's defect in
// the codegen pass. // per §15.4 (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           set_owning_set_new_inner_rejected_spec_15_4) {
    expectRejected(
        "    var xs: Set<OwningSet<Conn>> := [[new Conn(1)]]; return 0; ");
}

// `OwningDictionary<Conn, Conn>` is non-trivial-dispose at its VALUE
// position (the key borrows). `Dictionary<Conn, OwningDictionary<Conn, Conn>>`
// is the borrowing analogue; the inner literal at the owning dictionary's
// value position is the third fresh-producer form. The OUTER KEY is a
// borrowed `using var` binding (NOT a fresh producer), so the rejection
// can only come from the new third-form arm — if the new arm were
// reverted, this row would still pass via the outer-key rejection
// (the #2673 first arm), which is the wrong reason. The same borrowed-
// key pattern as `array_owning_array_borrowed_inner_rejected_spec_15_4`
// at line 184 of this file. // per §15.4 (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           dictionary_owning_dictionary_value_rejected_spec_15_4) {
    expectRejected(
        "    using var c := new Conn(7); "
        "    var d: Dictionary<Conn, OwningDictionary<Conn, Conn>> := "
        "        [c: [new Conn(1): new Conn(5)]]; "
        "    return 0; ");
}

// =====================================================================
// ACCEPT — the four neighbours §15.4 protects on the new arm.
// =====================================================================

// §15.4's literal-tree paragraph relies on this shape staying legal: an
// OWNING-family element is a fresh node of the literal tree, owned by the
// aggregate. `xs[0][0].Id` is the inner `Conn`'s Id. //
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           owning_array_owning_new_inner_admitted_spec_15_4) {
    expectAccepted(
        "    using var xs: OwningArray<OwningArray<Conn>> := [[new Conn(9)]]; "
        "    return xs[0][0].Id; ",
        9);
}

// §15.4's BOUNDARY on the new arm: a nested literal at a BORROWING-family
// target (`Array<Array<int>>`) is trivial-dispose (the borrowing family
// borrows the slot, doesn't own it), so the inner literal is NOT a fresh
// producer of non-trivial-dispose class type and the predicate returns
// false. The pre-existing #2673 test `nested_trivial_dispose_literal_admitted
// _spec_15_4` already pins this shape at the element-type level; this row
// pins it at the *target-type* level for the new arm. Outer `Length` = 2.
// per §15.4 (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           array_array_int_literal_admitted_spec_15_4) {
    expectAccepted(
        "    var g: Array<Array<int>> := [[1, 2], [3, 4]]; return g.Count; ",
        2);
}

// The trivially-dispose-bound nested literal at `Array<OwningArray<Box>>`.
// The inner literal TARGET is `OwningArray<Box>`, an owning-family head
// with a TRIVIAL-dispose class element (`Box` has no destructor and no
// `owned` field, so `OwningArray<Box>` is trivial-dispose), and the new
// arm's predicate returns false. The literal-tree ownership rule's "nested
// literal node" admission therefore carries the inner `[new Box(1)]`
// aggregate under the OUTER `OwningArray<Box>` slot — exactly as a
// trivial-dispose fresh element does (#2673's `trivial_dispose_class
// _fresh_element_admitted_spec_15_4_28_9` row). Outer `Count` = 2.
// per §15.4, §28.9 (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           array_owning_array_box_literal_admitted_spec_15_4) {
    expectAccepted(
        "    var g: Array<OwningArray<Box>> := [[new Box(1)], [new Box(2)]]; "
        "    return g.Count; ",
        2);
}

// The empty literal at the borrowing-family target is still admitted:
// §15.4 reads "the empty literal `[]` is admitted for every collection
// target listed above". The check's element loop is over an empty vector
// for an empty literal, so the new arm is a no-op — and stays one
// regardless of the element type. // per §15.4 (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           empty_array_owning_array_admitted_spec_15_4) {
    expectAccepted(
        "    var xs: Array<OwningArray<Conn>> := []; return xs.Count; ", 0);
}

// The borrowed element — the remedy's first half, executed. The element
// is an identifier (`a`), not a literal, so the new arm's predicate
// doesn't fire (the element is not a `CollectionLiteralExpr`), and the
// existing #2673 predicate doesn't fire either (an identifier is a borrow,
// not a fresh producer). The outer literal is admitted as a borrow of
// `a` exactly as #2673's `named_source_remedy_compiles_and_reads_back
// _spec_15_4` reads back through a single borrow. `b` is a second owned
// `OwningArray<Conn>` read back through the second element. Read-back: 22
// (11 + 11 from the inner Conn.Ids). // per §15.4, §28.11 (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           owned_locals_as_elements_admitted_spec_15_4) {
    expectAccepted(
        "    using var a := new OwningArray<Conn>(); "
        "    a.Add(new Conn(11)); "
        "    using var b := new OwningArray<Conn>(); "
        "    b.Add(new Conn(11)); "
        "    var xs: Array<OwningArray<Conn>> := [a, b]; "
        "    return xs[0][0].Id + xs[1][0].Id; ",
        22);
}

// The owning-family remedy, executed: same `[new Conn(1)]` shape that's
// REJECTED at `Array<OwningArray<Conn>>` is ACCEPTED at
// `OwningArray<OwningArray<Conn>>` (the owning family takes ownership and
// the literal-tree rule reclaims the inner free aggregate). Read-back: 9.
// per §15.4, §28.6 (#3307)
BEZEL_TEST(sema_borrowing_family_nested_literal_fresh_producer_3307,
           owning_array_owning_array_fresh_inner_admitted_spec_15_4) {
    expectAccepted(
        "    using var xs: OwningArray<OwningArray<Conn>> := [[new Conn(9)]]; "
        "    return xs[0][0].Id; ",
        9);
}
