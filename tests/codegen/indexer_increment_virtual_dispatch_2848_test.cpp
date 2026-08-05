// Spec-derived regression test for issue #2848.
//
// §17.6 (The increment and decrement operators, incl. "Increment/decrement on
// indexer slots") / §6.6 (Compound assignments) / §12.1 (Members are non-virtual
// by default) / §12.3 (`base.Member`) / §23.4 (Indexers), v0.240.
//
// §17.6 states, verbatim:
//
//     "For an indexer slot `expr[i]` (or `expr[i, j, ...]` for a multi-dim
//      indexer), `expr[i]++` desugars to a read-modify-write through the
//      indexer"
//
//     "Each subexpression in the LHS — the collection expression and every
//      index expression — is evaluated exactly once. The indexer's getter and
//      setter are each called once."
//
//     "The same desugaring applies to `--`. For `+=`, `-=`, and the other
//      compound assignments on indexer slots, the same desugaring applies with
//      the operator changed accordingly; it is the indexer instance of the
//      general compound-assignment rule of §6.6"
//
// So the spec does not merely LIKEN `a[i]++` to `a[i] += 1` — it defines the
// former as an instance of the latter's rule. §23.4 states, verbatim, that
// indexers "participate in `virtual`/`override`/`sealed`/`abstract` like
// methods."
//
// THE DEFECT. `lowerStmt`'s `ast::IncrementStmt` arm carried a ~480-line
// HAND-INLINED MIRROR of `lowerIndexerCompound` (its own comment said it
// "mirrors lowerIndexerCompound"), and the mirror had drifted three fixes behind
// the original:
//
//   1. RECEIVER (#2833). It read `ctx_.exprType(*ix->target)` directly. `this`
//      carries no `exprType`, so the whole block was skipped and the write was
//      DROPPED — no diagnostic, no store. MEASURED on b0fa41a: `this[0]++`
//      inside a base method on a derived instance left the field at 0, where 203
//      is correct. `lowerIndexerGet`/`-Set`/`-Compound` all call
//      `indexAccessReceiverDecl` instead. Every `this[i]++` shape below —
//      abstract, sealed, two-parameter, generic, lambda, async — was dropped for
//      this one reason.
//   2. DISPATCH (#2836). It had no class-vtable (#1080) and no interface-itable
//      (#730) branch, so a typed receiver bound STATICALLY: `b[0]++` through a
//      base-typed reference MEASURED 102 (the base's accessors) where 203 is
//      correct, and an interface-typed receiver — a bodiless PropertyDecl, so no
//      accessor body to inline — was dropped at 0.
//   3. `base[i]` (#2832). Not merely mis-dispatched: MEASURED 0, dropped, for
//      the same reason as (1) — `base` carries no `exprType` either.
//
// THE FIX deletes the mirror and routes `a[i]++` / `a[i]--` through the ONE
// definition, `lowerIndexerCompoundCore`, which `a[i] += 1` already used. The
// right operand for the `++` spelling is §17.6's implicit literal `1`,
// synthesized at the getter's result type because the desugaring has no AST node
// for it. Consolidation was chosen over patching the copy precisely because a
// third divergence is otherwise inevitable; the constraint that killed the last
// consolidation attempt (#2510) — §6.6's single evaluation — is preserved
// because it is a property of that one body, which lowers the receiver and every
// index ONCE into shared slots rather than driving `lowerIndexerGet` +
// `lowerIndexerSet`. T13 asserts that MECHANICALLY with a side-effecting index
// rather than trusting the structure.
//
// Every case asserts a CORRECT VALUE, not a clean compile: the defect compiled
// cleanly and produced the wrong number (or nothing at all). Constants are
// < 256 where the observable is the exit status (WEXITSTATUS-masked to 8 bits);
// larger observables go to stdout.
//
// Related: #2836 (the compound twin whose branches this now shares), #2833 (the
// receiver recovery), #2832 (`base[i]`), #1080, #730, #2509 (the imported-indexer
// branch, also now shared), #587, #613 (the built-in-collection arm, which still
// runs FIRST and is unaffected — T17), #2851 (a write to a getter-only indexer is
// silently discarded; unchanged here, T16).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAot;
using bezel::test::runAotOut;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

// The shared virtual/override pair. `B`'s accessors are +100/+1, `D`'s are
// +200/+2, so `this[0]++` on a `D` yields
//   * 203 when BOTH halves dispatch to D (correct),
//   * 102 when both bind statically to B (symptom B),
//   * 0   when the statement is dropped entirely (symptom A),
// three values that cannot be confused for one another.
constexpr const char* kPair =
    "class B "
    "  public _s: int := 0; "
    "  public virtual property [i: int]: int "
    "    get => _s + 100; "
    "    set => do _s := value + 1; end; "
    "  end; "
    "  public method Inc(): int "
    "    this[0]++; "
    "    return _s; "
    "  end Inc; "
    "  public method Dec(): int "
    "    this[0]--; "
    "    return _s; "
    "  end Dec; "
    "end B; "
    "class D: B "
    "  public override property [i: int]: int "
    "    get => _s + 200; "
    "    set => do _s := value + 2; end; "
    "  end; "
    "end D; ";

}  // namespace

// ---------------------------------------------------------------------------
// T1 — SYMPTOM A, the issue's first repro: `this[0]++` was DROPPED ENTIRELY.
// Pre-fix: 0, with no diagnostic. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           this_increment_dispatches_override_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc(); "
        "end Main; end C;")), 203);
}

// `--` is the same desugaring with the operator changed (§17.6: "The same
// desugaring applies to `--`"): D's getter gives 200, 200-1 = 199, D's setter
// stores 201. Pre-fix: 0.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           this_decrement_dispatches_override_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Dec(); "
        "end Main; end C;")), 201);
}

// ---------------------------------------------------------------------------
// T2 — SYMPTOM B, the issue's second repro: a BASE-TYPED receiver bound
// statically. §12.1. Pre-fix: 102. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           base_typed_receiver_dispatches_override_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); var b: B := d; b[0]++; return d._s; "
        "end Main; end C;")), 203);
}

// The DERIVED-typed receiver, which already agreed at 203 because the static
// type IS the runtime type. It is the reason the defect read as an
// inconsistency, and it must stay correct.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           derived_typed_receiver_stays_correct_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); d[0]++; return d._s; "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T3 — an INTERFACE-typed receiver (§23.4: "Indexers may be declared in
// interfaces with the body-less form"). Pre-fix: 0 — `pd` is the bodiless
// interface PropertyDecl, so the mirror found no accessor body. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           interface_typed_receiver_dispatches_itable_23_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "interface IBox property [i: int]: int; end IBox; "
        "class D: IBox "
        "  public _s: int := 0; "
        "  public property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); var q: IBox := d; q[0]++; return d._s; "
        "end Main; end C;")), 203);
}

// A TWO-PARAMETER interface indexer: the arity loop of the itable branch.
// Pre-fix: 0. Post-fix: 203.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           interface_two_parameter_indexer_23_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "interface IGrid property [r: int, c: int]: int; end IGrid; "
        "class D: IGrid "
        "  public _s: int := 0; "
        "  public property [r: int, c: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); var q: IGrid := d; q[2, 3]++; return d._s; "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T4 — an ABSTRACT read-write indexer incremented through `this[i]` inside the
// abstract base's own concrete method. §12.5: "`abstract` on a member declares
// it without a body". Pre-fix: 0. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           abstract_indexer_through_this_12_5) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "abstract class A "
        "  public _s: int := 0; "
        "  public abstract property [i: int]: int get; set; end; "
        "  public method Inc(): int "
        "    this[0]++; "
        "    return _s; "
        "  end Inc; "
        "end A; "
        "class D: A "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T5 — a TWO-PARAMETER class indexer (§23.4's `[row: int, col: int]` form) on
// the vtable branch, through `this`. Pre-fix: 0. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           two_parameter_indexer_dispatches_override_23_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [r: int, c: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Inc(): int "
        "    this[2, 3]++; "
        "    return _s; "
        "  end Inc; "
        "end B; "
        "class D: B "
        "  public override property [r: int, c: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T6 — inside a LAMBDA body (§27.3), and T7 — inside an ASYNC body (§30.1).
// These are the contexts #2833 made reachable for the compound spelling; for
// `++` they were still dropped, because the mirror never got #2833's receiver
// recovery. Pre-fix both: 0. Post-fix both: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           increment_inside_lambda_body_27_3) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Inc(): int "
        "    var f := lambda(n: int) => do this[0]++; end; "
        "    f(1); "
        "    return _s; "
        "  end Inc; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc(); "
        "end Main; end C;")), 203);
}

BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           increment_inside_async_body_30_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public async method Inc(): Task<int> "
        "    this[0]++; "
        "    return _s; "
        "  end Inc; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc().Result; "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T8 — a `sealed override` (§12.4: "`sealed` on an override prevents further
// overriding"). Sealing ends the chain but does NOT remove the slot, so the
// access through the BASE's method must still dispatch. Pre-fix: 0.
// Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           sealed_override_still_dispatches_12_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class DS: B "
        "  public sealed override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end DS; "
        "class C public static method Main(): int "
        "  using var ds := new DS(); return ds.Inc(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T9 — a three-level chain B -> D -> E: the access must reach the MOST-DERIVED
// override, not merely "one level down". E: get 300, +1 = 301, set stores 304.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           most_derived_override_in_a_chain_12_1) {
    const std::string prog =
        std::string("module M; ") + kPair +
        "class E: D "
        "  public override property [i: int]: int "
        "    get => _s + 300; "
        "    set => do _s := value + 3; end; "
        "  end; "
        "end E; "
        "class C public static method Main(): int ";
    // > 255, so the observable moves to stdout.
    BEZEL_EXPECT_TRUE(runAotOut(
        prog + "using var e := new E(); Console.WriteLine(e.Inc()); "
               "return 0; end Main; end C;") == "304");
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var d := new D(); return d.Inc(); end Main; end C;")),
        203);
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var b := new B(); return b.Inc(); end Main; end C;")),
        102);
}

// ---------------------------------------------------------------------------
// T10 — a GENERIC declaring class: the element and index types are read off the
// AST and must lower against the active substitution. Pre-fix: 0.
// Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           generic_declaring_class_dispatches_override_14_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B<T> "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Inc(): int "
        "    this[0]++; "
        "    return _s; "
        "  end Inc; "
        "end B; "
        "class D<T>: B<T> "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D<int>(); return d.Inc(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T11 — a `char` element (§5.2 / §6.6 #1961): the read-modify-write runs at int
// width and is narrowed back through the code-point check. D's getter yields
// 'a' (97), +1 = 98, D's setter stores 'b'. Pre-fix: 66 ('B' — B's getter read
// the raw field 'A' = 65 and B's setter stored 66). Post-fix: 98.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           char_element_dispatches_override_5_2) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: char := 'A'; "
        "  public virtual property [i: int]: char "
        "    get => _s; "
        "    set => do _s := value; end; "
        "  end; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: char "
        "    get => 'a'; "
        "    set => do _s := value; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); var b: B := d; b[0]++; "
        "  return int(d._s); "
        "end Main; end C;")), 98);   // 'b'
}

// ---------------------------------------------------------------------------
// T12 — a STRING-KEYED indexer, iterated: the index is a `+1` string PRODUCER
// (`"k" + i.ToString()`), which the shared body discharges once after the setter
// (#2717) — the mirror's imported branch never did. Both halves are asserted:
// the dispatched value, and that the index actually reached the setter.
//
// MEMORY (measured with LSan on, 5 iterations): NO leak, NO use-after-free, NO
// double free, before or after the fix.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           string_keyed_index_producer_dispatches_23_4) {
    const std::string prog =
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public _k: string := \"\"; "
        "  public virtual property [k: string]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; _k := k; end; "
        "  end; "
        "end B; "
        "class D: B "
        "  public override property [k: string]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; _k := k; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); var b: B := d; var i := 0; "
        "  while i < 5 do b[\"k\" + i.ToString()]++; i := i + 1; end; ";
    // Five dispatched round-trips: 203, 406, 609, 812, 1015. Statically bound
    // (pre-fix) it was 102, 204, 306, 408, 510.
    BEZEL_EXPECT_TRUE(runAotOut(
        prog + "Console.WriteLine(d._s); return 0; end Main; end C;") == "1015");
    BEZEL_EXPECT_TRUE(runAotOut(
        prog + "Console.WriteLine(d._k); return 0; end Main; end C;") == "k4");
}

// ---------------------------------------------------------------------------
// T13 — §6.6 / §17.6 SINGLE EVALUATION, asserted mechanically rather than
// assumed. This is the constraint that killed the previous consolidation
// attempt (#2510), so it is the one this change most owes a measurement.
//
// §17.6, verbatim: "Each subexpression in the LHS — the collection expression
// and every index expression — is evaluated exactly once. The indexer's getter
// and setter are each called once."
//
// The INDEX here is a call that increments an observable counter, so an index
// evaluated twice is visible as `N = 2`; the receiver is reached through a
// block-form property whose getter counts its own invocations, so a receiver
// evaluated twice is visible as `Hops = 2`.
//
// ⚠ The two COUNT assertions pass with and without the fix (the pre-fix mirror
// also evaluated each once); they are REGRESSION guards on the consolidation,
// which is exactly what they are for. The paired VALUE assertions are the
// fail-without halves (pre-fix: 102 and 0 respectively).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           index_expression_evaluated_exactly_once_17_6) {
    const std::string prog =
        std::string("module M; ") +
        "class Ctr "
        "  public N: int := 0; "
        "  public method Next(): int "
        "    N := N + 1; "
        "    return 0; "
        "  end Next; "
        "end Ctr; " + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); using var ctr := new Ctr(); "
        "  var b: B := d; "
        "  b[ctr.Next()]++; ";
    BEZEL_EXPECT_EQ(run(runAot(prog + "  return ctr.N; end Main; end C;")), 1);
    BEZEL_EXPECT_EQ(run(runAot(prog + "  return d._s; end Main; end C;")), 203);
}

BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           receiver_property_hop_invoked_exactly_once_6_6) {
    const std::string prog =
        std::string("module M; ") + kPair +
        "class Holder "
        "  public Hops: int := 0; "
        "  private _inner: B; "
        "  public constructor(b: B) "
        "    _inner := b; "
        "  end constructor; "
        "  public property Inner: B "
        "    get => do Hops := Hops + 1; return _inner; end; "
        "  end; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var d := new D(); using var h := new Holder(d); "
        "  h.Inner[0]++; ";
    BEZEL_EXPECT_EQ(run(runAot(prog + "  return h.Hops; end Main; end C;")), 1);
    BEZEL_EXPECT_EQ(run(runAot(prog + "  return d._s; end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// ACCEPT CONTROLS — shapes the fix must NOT change. A false dispatch is worse
// than the bug it fixes, and a consolidation is exactly the kind of change that
// can silently move an unrelated shape onto a different path.
// ---------------------------------------------------------------------------

// T14 — a NON-VIRTUAL indexer stays statically bound. §12.1: "A method,
// property, or indexer is non-virtual unless marked `virtual`." 102 before and
// after.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           non_virtual_indexer_stays_static_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(); b[0]++; return b._s; "
        "end Main; end C;")), 102);
}

// T15 — `base[i]++` must run the BASE's implementation. §12.3, verbatim:
// "Inside a derived member, `base.Name(...)` calls the base class's
// implementation, bypassing virtual dispatch." The base indexer shares its
// vtable slot with the derived override, so a dispatched `base[i]` would run
// the override (the #2832 defect).
//
// ⚠ This is a FAIL-WITHOUT case as well as a control: pre-fix `base[0]++` was
// DROPPED ENTIRELY (measured 0) because `base` carries no `exprType` either.
// Post-fix: 102 — the base's own accessors, dispatch correctly declined.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           base_keyword_increment_runs_base_body_12_3) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") +
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "  public method IncBase(): int "
        "    base[0]++; "
        "    return _s; "
        "  end IncBase; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.IncBase(); "
        "end Main; end C;")), 102);
}

// A RECORD receiver keeps the inline accessor-body path. §9: "A record cannot
// derive from another record or from a class", so a record indexer can never be
// virtual and has no vtable slot. 102 before and after.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           record_receiver_unchanged_9_23_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "record R "
        "  public V: int := 0; "
        "  public property [i: int]: int "
        "    get => V + 100; "
        "    set => do V := value + 1; end; "
        "  end; "
        "end R; "
        "class C public static method Main(): int "
        "  var r := new R(); r[3]++; return r.V; "
        "end Main; end C;")), 102);
}

// A `virtual` indexer the derived class does NOT override: the slot is filled
// with the base's own accessors, so dispatch and static binding must agree at
// 102. This is the shape a naive "if there is a slot, run the derived body" fix
// would break. 102 before and after.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           virtual_without_override_reaches_base_body_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class N: B end N; "
        "class C public static method Main(): int "
        "  using var n := new N(); var b: B := n; b[0]++; return n._s; "
        "end Main; end C;")), 102);
}

// An OVERLOAD SET. §23.4: "Indexers may be overloaded by parameter type list."
// The virtual `[i: int]` overload must dispatch (pre-fix 102 -> 203) while the
// non-virtual `[k: string]` overload stays static (506 before and after) — the
// vtable slots are keyed by index signature (#1080), so selecting the wrong one
// would show up here.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           overload_set_selects_per_signature_23_4) {
    const std::string prog =
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public _t: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public property [k: string]: int "
        "    get => _t + 500; "
        "    set => do _t := value + 5; end; "
        "  end; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); var b: B := d; "
        "  b[0]++; b[\"k\"]++; ";
    BEZEL_EXPECT_EQ(run(runAot(prog + "return d._s; end Main; end C;")), 203);
    // > 255, so the observable moves to stdout.
    BEZEL_EXPECT_TRUE(runAotOut(
        prog + "Console.WriteLine(d._t); return 0; end Main; end C;") == "506");
}

// T16 — a GETTER-ONLY (expression-bodied) virtual indexer. §23.2's `=>` form is
// read-only by shape, so there is no setter to write through; the write is
// SILENTLY DISCARDED. That is a pre-existing sema gap (#2851 — the correct
// answer is a rejection, not a lowering), and this change deliberately
// preserves the outcome rather than papering over it: 7 before and after.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           getter_only_indexer_write_unchanged_23_2) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 7; "
        "  public virtual property [i: int]: int => _s + 100; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(); b[0]++; return b._s; "
        "end Main; end C;")), 7);
}

// T17 — the BUILT-IN heap-collection arm (#613 / stdlib §4.4 / §4.8) still runs
// FIRST and is untouched by the consolidation: `Array<T>` and
// `Dictionary<K, V>` have no user PropertyDecl and never reach the user-indexer
// arm at all. 6 and 8 before and after.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           builtin_collection_increment_unchanged_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(); arr.Add(5); arr[0]++; "
        "  return arr[0]; "
        "end Main; end C;")), 6);
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); d[\"k\"] := 7; "
        "  d[\"k\"]++; return d[\"k\"]; "
        "end Main; end C;")), 8);
}

// T18 — a THROW out of the dispatched accessor (§31.1/§31.2). The shared body
// calls `emitPendingCheck` after each dispatched call, so the exception reaches
// the enclosing handler and NO store happens on that edge; the normal path in
// the same program still stores the dispatched result.
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           throw_from_dispatched_accessor_31_1) {
    const std::string decls =
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: int "
        "    get => do "
        "      if i = 9 then throw new ArgumentException(\"bad\"); end; "
        "      return _s + 200; "
        "    end; "
        "    set => do "
        "      if value > 1000 then throw new ArgumentException(\"big\"); end; "
        "      _s := value + 2; "
        "    end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); var b: B := d; ";
    // The GETTER throws: nothing is stored.
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "  try b[9]++; catch ArgumentException => return d._s; end; "
                "  return 250; end Main; end C;")), 0);
    // The same accessors on the normal path still dispatch and store.
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "  b[0]++; return d._s; end Main; end C;")), 203);
}

// T19 — a RECORD reached through an INTERFACE-typed variable: the itable branch
// can land on a record-backed implementor. §9 value semantics mean the write
// lands in the BOX, not in `r`, so both halves are asserted — reading back
// through the interface sees the stored value while the original record is
// untouched. (This mirrors #2836's T22 for the `++` spelling.)
BEZEL_TEST(codegen_indexer_increment_virtual_2848,
           record_backed_interface_receiver_9_23_4) {
    const std::string prog =
        "module M; "
        "interface IBox property [i: int]: int; end IBox; "
        "record R: IBox "
        "  public V: int := 0; "
        "  public property [i: int]: int "
        "    get => V + 200; "
        "    set => do V := value + 2; end; "
        "  end; "
        "end R; "
        "class C public static method Main(): int "
        "  var r := new R(); var q: IBox := r; q[0]++; ";
    // > 255, so the observable moves to stdout.
    BEZEL_EXPECT_TRUE(runAotOut(
        prog + "Console.WriteLine(q[0]); return 0; end Main; end C;") == "403");
    BEZEL_EXPECT_EQ(run(runAot(prog + "return r.V; end Main; end C;")), 0);
}
