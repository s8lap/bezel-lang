// fix #6404 / §28.6 (spec v0.338): a getter's return was classified on the
// RETURNED EXPRESSION, and an `IdentifierExpr` reads Borrowed — so ONE local
// laundered a freshly-produced owned value past §28.6.
//
// §28.6 verbatim: "A **property or indexer getter** may not return a
// freshly-produced owned value (axiom A4). A getter is a borrow-producing
// position — properties and indexers do not carry `owned` (§23; the `owned`
// modifier applies only to fields), so its return is always a borrow — and a
// fresh owned value returned through it would escape with no owner and leak.
// Expose the factory as a **method returning `owned T`** instead, which can
// hand the caller ownership."
//
// The rule is about the VALUE and NAMES ITS PRODUCERS — "`new` expression, a
// call returning `owned T`, a `move` expression, or an owned operator result" —
// so a local minted from one of those and returned is the same escape; only the
// syntactic form differed. Measured before the fix: `return new SelfA(3);` was
// REJECTED, and `var t := new SelfA(3); return t;` compiled CLEAN (exit 0).
// The fix follows a returned BINDING back through the chain of locals the
// getter body declared.
//
// The ACCEPT rows are the load-bearing half: each is a shape a too-eager peel
// would reject. `reassigned_local_discharged_accepted` pins the §28.7
// drop-on-overwrite stop, `field_read_through_local_accepted` and
// `owning_collection_field_through_local_accepted` pin the borrow rows §28.6's
// FIRST bullet admits (K9.1's lending read), and
// `owned_task_field_through_local_accepted` pins §28.3's stored-frame
// re-exposure face — `producerState` classifies a READ of task-typed storage as
// a Borrow ("storage lends"), so the peel must stop there too.
//
// THE MINT-ANNOUNCING CARVE, ITS KEY, AND THE LOCAL SPELLINGS (review of the
// first attempt, which was rejected for keying the carve on an EXPRESSION's
// type). §28.6's rule is about the VALUE a getter returns, so the carve §28.6
// bullet 2 carves for the mint-announcing families has to be read off the
// getter's own DECLARED RETURN TYPE — §28.3's "Returning a deferred-activation
// frame": "The classification stays by declared return type". The admitted set
// is that section's announcement-table ROW 2 (v0.338): a `Task`-family type or
// an iterable-family type (one of the eight §29.3/§30.7 heads, or a lazy
// query). `concrete_owning_collection_head_frame_rejected` and
// `owning_collection_frame_getter_accepted` are the byte-identical-bodies
// discriminator pair for that key; `task_family_frame_getter_accepted` and
// `plain_iterable_frame_getter_accepted` cover the other two arms.
// `const_local_laundering_rejected` / `const_local_chain_rejected` cover
// `ast::LocalConstDecl` (§21.1 *Runtime `const` locals*), the declaration node
// the first attempt's `declared` set could not see, and
// `self_assignment_is_not_a_discharge_rejected` covers `t := t;`, which is a
// no-op and not the §28.7 discharge `reassigned_local_discharged_accepted`
// relies on.
// per §28.6, §28.3, §28.7, §21.1, §15.3, K6.2, K9.1, #6404

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The §28.6 needle. A STABLE rule identifier (§-reference plus the key term),
// so a wording tweak that rephrases the sentence does not silently break the
// row while an unrelated rule firing cannot satisfy it.
constexpr const char* kGetterEscape =
    "a property or indexer getter may not return a freshly-produced owned value";

const char* kPreamble = R"(
module M;
class Cell end Cell;
class Factory
  private _cell: Cell?;
  private owned _items: OwningArray<Cell>;
  private owned _task: Task<int>;
  public constructor(c: Cell?)
    _cell := c;
    _items := new OwningArray<Cell>();
    _task := Task.FromResult<int>(3);
  end constructor;
)";

// ── the defect: one local launders a fresh owned value past §28.6 ──────────
// The issue's actual repro shape, in miniature.
BEZEL_TEST(getter_return_binding_provenance_6404,
           one_local_laundering_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      var t := new Cell();
      return t;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterEscape));
}

// The direct spelling — the control that makes the laundering row non-vacuous.
// REJECTED before #6404 too; asserted here so the pair pins both ends of the
// same rule, not just the arm the fix added.
BEZEL_TEST(getter_return_binding_provenance_6404,
           direct_fresh_owned_return_still_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      return new Cell();
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterEscape));
}

// Laundering through SEVERAL locals — the chain peel's reason for existing.
// A one-local-only rule would leave this open.
BEZEL_TEST(getter_return_binding_provenance_6404,
           several_local_chain_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      var a := new Cell();
      var b := a;
      var c := b;
      return c;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterEscape));
}

// `using var t := new Cell(); return t;` — §28.1 admits no `owned`-declared
// LOCAL other than the `using var` form, so this IS the "owned-declared local"
// spelling. The using var is the scope's only owner; returning it escapes.
BEZEL_TEST(getter_return_binding_provenance_6404, using_var_local_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      using var t := new Cell();
      return t;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterEscape));
}

// A laundering return nested in a control-flow arm of the getter body.
BEZEL_TEST(getter_return_binding_provenance_6404, if_branch_return_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public method Always(): bool return true; end Always;
  public property Item: Cell?
    get => do
      var t := new Cell();
      if Always() then return t; else return _cell; end if;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterEscape));
}

// `return move t;` — measured ALREADY rejected before #6404 (producerState
// classifies a `move` of a class-typed local Owned). Asserted as a regression
// control: the binding peel must not have cost this arm.
BEZEL_TEST(getter_return_binding_provenance_6404, move_local_still_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      var t := new Cell();
      return move t;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterEscape));
}

// ── the ACCEPT rows: shapes the peel must NOT reach ───────────────────────

// A field read through a local is a BORROW (§28.6 bullet 1 / K9.1). The peel
// stops at the local's initializer because it is a MemberAccessExpr, not an
// identifier — this row is what proves "not an identifier ⇒ stop" is not
// really "not owned ⇒ stop".
BEZEL_TEST(getter_return_binding_provenance_6404,
           field_read_through_local_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      var x := _cell;
      return x;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An owning-collection `owned` field through a local — §28.6 bullet 1 admits
// exactly this: "A bare read of an owned field lends (K9.1) and may be
// returned through a non-`owned` return type as a borrow of the field …
// uniformly so for a class-typed owned field and for an owning-collection
// field".
BEZEL_TEST(getter_return_binding_provenance_6404,
           owning_collection_field_through_local_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Items: OwningArray<Cell>
    get => do
      var x := _items;
      return x;
    end;
  end Items;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3's stored-frame re-exposure face: a `Task`-family field re-exposed
// through a property. producerState reads task-typed STORAGE as a Borrow
// ("storage lends" — a Task-typed read never reaches the CallExpr arm), so the
// peel must stop on it exactly as on a plain field.
BEZEL_TEST(getter_return_binding_provenance_6404,
           owned_task_field_through_local_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Task_: Task<int>
    get => do
      var x := _task;
      return x;
    end;
  end Task_;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.7 drop-on-overwrite DISCHARGES the displaced fresh value at the store, so
// the local holds a borrow of `_cell` by the time it is returned. Following the
// declaration alone would reject this legal program.
BEZEL_TEST(getter_return_binding_provenance_6404,
           reassigned_local_discharged_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell();
      t := _cell;
      return t;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A getter returning a plain non-owned field read — the baseline legal form.
BEZEL_TEST(getter_return_binding_provenance_6404,
           bare_field_read_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell? get => _cell; end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.6 bullet 2's mint-announcing carve, pinned. A getter typed at an
// iterable-interface head re-exposing a FRESH owning frame through a local is
// the shape #3355's `block_bodied_getter_owning_array_no_segv_29_2` uses (its
// caller discharges the elements with `using var y := owned x;`), and the
// carve is what keeps that row green — without it the peel rejects it and flips
// a pinned test. This row is the carve's own non-vacuity check: remove
// getterEscapeIsMintAnnouncingFrame and both it and #3355 go red.
BEZEL_TEST(getter_return_binding_provenance_6404,
           owning_collection_frame_getter_accepted) {
    auto r = parseAndCheckOwnership(R"(
module M;
class Item
  public V: int;
  public constructor(v: int) V := v; end constructor;
end Item;
class C
  public property Items: IOwningEnumerable<Item>
    get => do
      var a := new OwningArray<Item>();
      a.Add(new Item(5));
      return a;
    end;
  end Items;
end C;
class Main public static method Main(): int return 0; end Main; end Main;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── the carve's KEY, and the set it admits (#6404 review, parts 1 and 2) ─────

// The carve is keyed on the GETTER'S OWN DECLARED RETURN TYPE, and the admitted
// set is §28.6's stated union — `Task` ∪ iterable, which §28.3's announcement
// table ROW 2 states normatively (v0.338): "a `Task`-family type … or an
// iterable-family type: one of the eight §29.3/§30.7 iterable/enumerator
// interface heads, or a lazy query".
//
// This row is the DISCRIMINATOR that keeps the key honest. The BODY is
// byte-identical to `owning_collection_frame_getter_accepted` — same `new
// OwningArray<Item>()` producer, same local, same single-hop peel. The ONLY
// difference is the declared return type: `IOwningEnumerable<Item>` (in the
// carve) here, `OwningArray<Item>` (NOT in the carve) in the twin below. If
// the carve were keyed on anything about the BODY — on the producer
// expression's type, which is `OwningArray<Item>` in BOTH programs — it would
// accept both. One local would then decide the verdict, which is exactly the
// defect the review named.
//
// The rejection is §28.6's own verdict for this spelling, and the spec writes
// it out: "a freshly-produced owning value (`return new OwningArray<T>(...)`)
// returned through a non-`owned` return is likewise an error", illustrated by
// `method LeakTransactions(): OwningArray<Transaction> return new
// OwningArray<Transaction>(); // ERROR`. A concrete owning collection is §28.3
// announcement-table row 4 — an ordinary borrowed reference, never row 2's
// mint-announcing frame — so §28.6 bullet 2's parenthetical does not reach it.
// per §28.6, §28.3 (announcement table rows 2 and 4), v0.338
BEZEL_TEST(getter_return_binding_provenance_6404,
           concrete_owning_collection_head_frame_rejected) {
    auto r = parseAndCheckOwnership(R"(
module M;
class Item
  public V: int;
  public constructor(v: int) V := v; end constructor;
end Item;
class C
  public property Items: OwningArray<Item>
    get => do
      var a := new OwningArray<Item>();
      a.Add(new Item(5));
      return a;
    end;
  end Items;
end C;
class Main public static method Main(): int return 0; end Main; end Main;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterEscape));
}

// The Task half of the union, on its own. `isTaskFamilyHead` is the `Task`
// family of §28.3 announcement-table row 2, and `OwningTask` sits in BOTH
// registry sets — so this row and the `OwningTask` twin below are what keep the
// `isTaskFamilyHead` arm non-vacuous independently of the iterable arm.
BEZEL_TEST(getter_return_binding_provenance_6404,
           task_family_frame_getter_accepted) {
    auto r = parseAndCheckOwnership(R"(
module M;
class C
  public property T: Task<int>
    get => do
      var a := Task.FromResult<int>(3);
      return a;
    end;
  end T;
end C;
class Main public static method Main(): int return 0; end Main; end Main;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `IEnumerable<Item>` — the plain (non-owning) member of the eight iterable
// heads. Its twin above is one of the four OWNING iterable heads, which live in
// `isIterableInterfaceHead` AND (before this fix) in `isOwningTypeHead`; this
// row is the one that stays green only because the key is the HEAD and not the
// owning/borrowing distinction.
BEZEL_TEST(getter_return_binding_provenance_6404,
           plain_iterable_frame_getter_accepted) {
    auto r = parseAndCheckOwnership(R"(
module M;
class Item
  public V: int;
  public constructor(v: int) V := v; end constructor;
end Item;
class C
  public property Items: IEnumerable<Item>
    get => do
      var a := new OwningArray<Item>();
      a.Add(new Item(5));
      return a;
    end;
  end Items;
end C;
class Main public static method Main(): int return 0; end Main; end Main;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── the local spellings, enumerated (#6404 review, part 3) ──────────────────

// A `const` local (§21.1 *Runtime `const` locals*, A.6 `LocalConstDecl`) is a
// DIFFERENT AST node from `ast::LocalDecl` and holds its initializer in `value`,
// not `initializer`. A peel that collected only `LocalDecl` never saw it, so
// this shape laundered. Measured ACCEPTED before this row.
//
// A runtime `const` is single-assignment — §21.1: "A `const` local of either
// classification is not an assignable location: assignment, compound
// assignment, `++`/`--`, and binding to a `ref` or `out` parameter are compile
// errors naming the declaration" — so the §28.7 reassignment discharge the
// `var` rows rely on can never apply to one: there is no spelling that retires
// the fresh value.
BEZEL_TEST(getter_return_binding_provenance_6404,
           const_local_laundering_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      const t := new Cell();
      return t;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterEscape));
}

// The same `const` spelling through a CHAIN, and with the written `: Type`
// annotation (A.6 admits `const Identifier (":" Type)? ":=" ConstExpr ";"`) —
// so the initializer is reached through both shapes of `LocalConstDecl`.
BEZEL_TEST(getter_return_binding_provenance_6404,
           const_local_chain_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      const t: Cell? := new Cell();
      const u := t;
      return u;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterEscape));
}

// `t := t;` is a NO-OP, not a §28.7 discharge. §28.7 disposes "the previous
// value" when the RHS is a DIFFERENT value; here the RHS IS the previous
// value, so nothing is retired and the local still holds the fresh `new` at the
// `return`. The spec's stated self-assignment no-op is §15.3's, and it is
// scoped there to whole-value reassignment of a MOVE-ONLY value type — not to
// a plain-`var` owning-classified local. The compiler already implements it as
// a no-op for such a local (ownership's `recollectAliasBorrowSourceOnReassign`
// header; codegen's retain-before-release at every drop-on-overwrite site), so
// accepting this would be accepting a fresh-value escape. Measured ACCEPTED
// before this row.
BEZEL_TEST(getter_return_binding_provenance_6404,
           self_assignment_is_not_a_discharge_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell();
      t := t;
      return t;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterEscape));
}

// The self-assignment exclusion must NOT swallow §21.1's own const rule. Here
// `t := t` on a `const` local is still the const-assignment error §21.1 states
// (measured on the fixed compiler), NOT the §28.6 getter diagnostic — the two
// positions are distinct rules and each must fire as itself.
BEZEL_TEST(getter_return_binding_provenance_6404,
           const_local_self_assignment_names_21_1) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      const t := new Cell();
      t := t;
      return t;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "cannot assign to const local"));
}

// The ACCEPT control for the self-assignment clause: a self-store is a no-op,
// so it can neither CREATE an escape nor be read as one. The same spelling over
// a local minted from a FIELD READ stays legal — the peel stops at the field
// (§28.6 bullet 1 / K9.1 lending read) exactly as it does without the
// self-store, so the clause adds a rejection only where a fresh producer is
// actually reachable.
BEZEL_TEST(getter_return_binding_provenance_6404,
           self_assignment_over_a_borrow_stays_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Item: Cell?
    get => do
      var t: Cell? := _cell;
      t := t;
      return t;
    end;
  end Item;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The residual #6404 leaves OPEN — deliberately NOT pinned as an ACCEPT row.
// The peel ends at a non-identifier returned expression, and a `??` WRAPS the
// binding rather than naming it (`var t: Cell? := new Cell(); return t ?? _c;`),
// so that shape is still admitted. §28.3's `a ?? b` row calls the compound the
// MEET of its arms, mixed = REJECTED, so whether this shape SHOULD be admitted
// turns on whether the local read `t` is Owned at that position — a spec
// question this fix does not settle. Pinning it green here would freeze a
// possibly-defective verdict as the contract; pinning it rejected would assert
// a ruling nobody has made. It is measured and reported instead.

} // namespace