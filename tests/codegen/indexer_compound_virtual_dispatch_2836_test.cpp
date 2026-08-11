// Spec-derived regression test for issue #2836.
//
// §12.1 (Members are non-virtual by default) / §23.4 (Indexers) / §6.6
// (Compound assignments) / §12.3 (`base.Member`), v0.240.
//
// §23.4 states, verbatim, that indexers
//
//     "participate in `virtual`/`override`/`sealed`/`abstract` like methods."
//
// and §6.6 states, verbatim (the document's hard line wraps joined with a
// single space here and in every §6.6 quotation below), that
//
//     "the statement `x op= e` is equivalent to `x := x op e` with the target
//      evaluated **once**"
//
// with the itemised guarantee that
//
//     "when the final segment is accessor-mediated (a property or an indexer),
//      its getter and setter are each invoked exactly once — the getter
//      produces the left operand, the setter stores the result".
//
// THE DEFECT. `a[i] OP= v` through a VIRTUAL indexer bypassed vtable dispatch
// and ran the COMPILE-TIME receiver type's accessor bodies. `lowerIndexerGet`
// and `lowerIndexerSet` (src/codegen/LlvmBackend_Assign.cpp) each carry an
// interface-itable branch (#730) and a class-vtable branch (#1080);
// `lowerIndexerCompound` in the same file had NEITHER, so its local
// (`pd != nullptr`) path went straight to "find a getter accessor body / find a
// setter accessor body" and inlined the static type's bodies. The plain read
// `a[i]` and the plain write `a[i] := v` right next to it dispatched correctly —
// only the compound form did not.
//
// MEASURED on 9983ddb (the base this fix was written against), the issue's own
// repro: 102, where 203 is correct. Compiles with NO diagnostic. Two further
// shapes were not merely wrong but DROPPED ENTIRELY (measured 0, i.e. the
// statement did nothing at all): an INTERFACE-typed receiver, and an `abstract`
// read-write indexer compounded through `this[i]` inside the abstract base's own
// method — for both, `pd` is a bodiless PropertyDecl, so the accessor-body
// search found nothing and the function returned false.
//
// WHY IT BECAME URGENT. The defect is pre-existing for every receiver, but
// #2833 (`indexAccessReceiverDecl`) made the whole `this[i]` path reachable from
// a LAMBDA body and an `async` body, where it had previously been dropped with a
// warning. In those contexts the shape went from "wrong, but announced" to
// "wrong and silent" — see T6/T7, which are exactly those two bodies.
//
// THE FIX adds the two missing branches to `lowerIndexerCompound`, mirroring its
// two siblings one for one, and widens that function's `!pd` gate to exclude an
// INTERFACE receiver exactly as #2422 widened theirs. Both new branches evaluate
// the receiver and every index expression ONCE and share them between the getter
// call and the setter call, in §6.6's stated order — they do NOT delegate to
// `lowerIndexerGet` + `lowerIndexerSet`, which would evaluate both twice (the
// #2510 shape, correctly declined). T10 asserts the evaluate-once contract
// mechanically with a side-effecting index rather than trusting the structure.
//
// Every case asserts a CORRECT VALUE, not a clean compile: the defect compiled
// cleanly and produced the wrong number (or nothing at all).
//
// Constants are all < 256 (runAot's exit status is WEXITSTATUS-masked to 8
// bits) and chosen so the correct result (203) can never coincide with the
// statically-bound wrong result (102) or with a dropped write (0).
//
// Related: #1080 (the class-vtable branches the compound path lacked), #730 (the
// itable branches), #2832 (`base[i]` must NOT dispatch — T12), #2833 (the
// receiver recovery that made T6/T7 reachable), #2509 (the imported `!pd`
// branch, whose gate this fix widens — its cross-module twin is
// tests/module/indexer_compound_iface_import_2836_test.cpp), #2847 (the string
// leak the interface branch still carries, recorded not hidden).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAot;
using bezel::test::runAotOut;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

// The shared virtual/override pair. `B`'s accessors are +100/+1, `D`'s are
// +200/+2, so `this[0] += 1` yields
//   * 203 when BOTH halves dispatch to D (correct),
//   * 102 when both bind statically to B (the defect),
//   * 0    when the statement is dropped entirely,
// three values that cannot be confused for one another.
constexpr const char* kPair =
    "class B "
    "  public _s: int := 0; "
    "  public virtual property [i: int]: int "
    "    get => _s + 100; "
    "    set => do _s := value + 1; end; "
    "  end; "
    "  public method Bump(): int "
    "    this[0] += 1; "
    "    return _s; "
    "  end Bump; "
    "end B; "
    "class D: B "
    "  public override property [i: int]: int "
    "    get => _s + 200; "
    "    set => do _s := value + 2; end; "
    "  end; "
    "end D; ";

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO — `this[i] += 1` inside a base-class method on a
// derived instance. §12.1 / §23.4. Pre-fix: 102. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           this_compound_dispatches_override_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Bump(); "
        "end Main; end C;")), 203);
}

// The two NON-compound spellings of the same access, which already dispatched
// correctly — they are the reason the defect reads as an inconsistency rather
// than a missing feature, and they must stay correct.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           plain_read_and_write_stay_virtual_23_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); return d[0]; "     // D's getter
        "end Main; end C;")), 200);
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); d[0] := 7; return d._s; "  // D's setter
        "end Main; end C;")), 9);
}

// ---------------------------------------------------------------------------
// T2 — an ORDINARY typed receiver, not `this`: the issue's "the same gap
// applies to an ordinary typed receiver" scope note. A BASE-typed reference to
// a derived instance. Pre-fix: 102. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           base_typed_reference_dispatches_override_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); var b: B := d; b[0] += 1; return d._s; "
        "end Main; end C;")), 203);
}

// A DERIVED-typed receiver is the accept control for T2: static binding and
// dynamic dispatch coincide there, so it was already 203 pre-fix and must
// remain 203 — proof the fix did not merely move the wrongness.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           derived_typed_reference_unchanged_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); d[0] += 1; return d._s; "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T3 — an INTERFACE-typed receiver (§11.2 / §23.4's "Indexers may be declared
// in interfaces with the body-less form"). The compound was DROPPED ENTIRELY —
// `pd` is the bodiless interface PropertyDecl, so no accessor body was found.
// Pre-fix: 0. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
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
        "  using var d := new D(); var q: IBox := d; q[0] += 1; return d._s; "
        "end Main; end C;")), 203);
}

// The interface branch must also carry a GENERIC element type through the
// receiver instantiation (#2528's resolution step, which the sibling branches
// perform and this one reuses).
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           interface_generic_element_23_4_14_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "interface IBag<T> property [i: int]: T; end IBag; "
        "class D: IBag<int> "
        "  public _s: int := 0; "
        "  public property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); var q: IBag<int> := d; q[0] += 1; "
        "  return d._s; "
        "end Main; end C;")), 203);
}

// A TWO-PARAMETER interface indexer: the arity loop of the new itable branch.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
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
        "  using var d := new D(); var q: IGrid := d; q[2, 3] += 1; "
        "  return d._s; "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T4 — an ABSTRACT read-write indexer, compounded through `this[i]` inside the
// abstract base's own concrete method. §12.5: "`abstract` on a member declares
// it without a body; every concrete subclass must override it." The base has no
// accessor body to inline, so pre-fix this too was DROPPED: 0. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           abstract_indexer_through_this_12_5) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "abstract class A "
        "  public _s: int := 0; "
        "  public abstract property [i: int]: int get; set; end; "
        "  public method Bump(): int "
        "    this[0] += 1; "
        "    return _s; "
        "  end Bump; "
        "end A; "
        "class D: A "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Bump(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T5 — a TWO-PARAMETER class indexer (§23.4's `[row: int, col: int]` form) on
// the vtable branch. Pre-fix: 102. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           two_parameter_indexer_dispatches_override_23_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [r: int, c: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Bump(): int "
        "    this[2, 3] += 1; "
        "    return _s; "
        "  end Bump; "
        "end B; "
        "class D: B "
        "  public override property [r: int, c: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Bump(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T6 — inside a LAMBDA body (§27.3). This is the newly-reachable context: before
// #2833 the whole access was dropped and ANNOUNCED; after it, the access lowers
// and the wrong value is fully SILENT. Pre-fix (on 9983ddb, i.e. WITH #2833):
// 102. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           compound_inside_lambda_body_27_3) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Bump(): int "
        "    var f := lambda(n: int) => do this[0] += n; end; "
        "    f(1); "
        "    return _s; "
        "  end Bump; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Bump(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T7 — inside an ASYNC body (§30.1): a state-machine frame, not a closure. The
// other half of the newly-reachable surface. Pre-fix: 102. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           compound_inside_async_body_30_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public async method Bump(): Task<int> "
        "    this[0] += 1; "
        "    return _s; "
        "  end Bump; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Bump().Result; "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T8 — a `sealed override` (§12.4: "`sealed` on an override prevents further
// overriding"). Sealing ends the chain but does NOT remove the slot, so the
// access must still dispatch. Pre-fix: 102. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           sealed_override_still_dispatches_12_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Bump(): int "
        "    this[0] += 1; "
        "    return _s; "
        "  end Bump; "
        "end B; "
        "class D: B "
        "  public sealed override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Bump(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T9 — the operator axis. §6.6 lists `+=`, `-=`, `*=`, `&=`, `|=`, `^=`, `<<=`,
// `>>=`; the dispatch is operator-independent, so one non-`+` case per family
// pins that the new branch is not `+`-specific. `-=`: D's getter gives 200,
// 200-1 = 199, D's setter stores 199+2 = 201. Pre-fix: 100 (B's 100-1 = 99,
// B's setter stores 99+1 = 100).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           subtract_assign_dispatches_override_6_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Bump(): int "
        "    this[0] -= 1; "
        "    return _s; "
        "  end Bump; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Bump(); "
        "end Main; end C;")), 203 - 2);
}

// The bitwise / shift families over the same dispatch. D's getter is a constant
// 1000-relative value so each operator's result is distinct from every other's
// and from the statically-bound answer.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           bitwise_and_shift_dispatch_override_6_6) {
    const std::string pre =
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s; "
        "    set => do _s := value; end; "
        "  end; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); var b: B := d; ";
    const std::string post = " return d._s; end Main; end C;";
    // 100 | 7 = 103, setter stores 104.  (statically bound: 0|7 = 7 → 7)
    BEZEL_EXPECT_EQ(run(runAot(pre + "b[0] |= 7;" + post)), 104);
    // 100 ^ 5 = 97, setter stores 98.    (statically bound: 0^5 = 5 → 5)
    BEZEL_EXPECT_EQ(run(runAot(pre + "b[0] ^= 5;" + post)), 98);
    // 100 << 1 = 200, setter stores 201. (statically bound: 0<<1 = 0 → 0)
    BEZEL_EXPECT_EQ(run(runAot(pre + "b[0] <<= 1;" + post)), 201);
    // 100 * 2 = 200, setter stores 201.  (statically bound: 0*2 = 0 → 0)
    BEZEL_EXPECT_EQ(run(runAot(pre + "b[0] *= 2;" + post)), 201);
}

// ---------------------------------------------------------------------------
// T10 — §6.6 SINGLE EVALUATION, asserted mechanically rather than assumed.
//
// §6.6, verbatim: "every constituent of the left-hand side's lvalue chain
// (grammar A.6 `LValue`) is evaluated exactly once, left to right: the base
// identifier or `this`, each mid-chain member hop (a property hop's getter is
// invoked once as part of locating the target), and every index expression in
// the chain".
//
// This is the contract that ruled out delegating the compound path to
// lowerIndexerGet + lowerIndexerSet (#2510). The INDEX here is a call that
// increments an observable counter, so an index evaluated twice is visible as
// `N = 2`.
//
// ⚠ The evaluate-once ASSERTION passes with and without the fix (the pre-fix
// inline path also evaluated the index once); it is a REGRESSION guard on the
// new branches, which is exactly what it is for. The paired value assertion
// below is the fail-without half (pre-fix: 102).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           index_expression_evaluated_exactly_once_6_6) {
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
        "  b[ctr.Next()] += 1; ";
    // The index expression ran exactly once …
    BEZEL_EXPECT_EQ(run(runAot(prog + "  return ctr.N; end Main; end C;")), 1);
    // … and the single evaluation still produced the dispatched result.
    BEZEL_EXPECT_EQ(run(runAot(prog + "  return d._s; end Main; end C;")), 203);
}

// The mid-chain MEMBER HOP half of the same §6.6 sentence: "a property hop's
// getter is invoked once as part of locating the target". The receiver is
// reached through a block-form property whose getter counts its invocations.
// Same split as above: the hop-count assertion is the regression guard, the
// value assertion is the fail-without half (pre-fix: 102).
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
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
        "  h.Inner[0] += 1; ";
    BEZEL_EXPECT_EQ(run(runAot(prog + "  return h.Hops; end Main; end C;")), 1);
    BEZEL_EXPECT_EQ(run(runAot(prog + "  return d._s; end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// ACCEPT CONTROLS — shapes the fix must NOT change. A false dispatch is worse
// than the bug it fixes: these are the four ways this change could break
// working code.
// ---------------------------------------------------------------------------

// T11 — a NON-VIRTUAL indexer stays statically bound. §12.1: "A method,
// property, or indexer is non-virtual unless marked `virtual`." 102 before and
// after. (This is also the shape #2833's own T6 pins from the other side.)
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           non_virtual_indexer_stays_static_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Bump(): int "
        "    this[0] += 1; "
        "    return _s; "
        "  end Bump; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(); return b.Bump(); "
        "end Main; end C;")), 102);
}

// T12 — `base[i] OP= v` must NOT dispatch. §12.3, verbatim: "Inside a derived
// member, `base.Name(...)` calls the base class's implementation, bypassing
// virtual dispatch." The base indexer shares its vtable slot with the derived
// override, so a dispatched `base[i]` would run the override — the #2832
// defect, one level down. 102 before and after.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           base_keyword_compound_stays_non_virtual_12_3) {
    BEZEL_EXPECT_EQ(run(runAot(
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
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "  public method BumpBase(): int "
        "    base[0] += 1; "
        "    return _s; "
        "  end BumpBase; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.BumpBase(); "
        "end Main; end C;")), 102);
}

// T13 — a RECORD receiver keeps the inline accessor-body path. §9: "A record
// cannot derive from another record or from a class", so a record indexer can
// never be virtual and has no vtable slot; §23.4 admits the read-write form on
// a body-form record with mutable properties. 106 before and after.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
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
        "  var r := new R(); r[3] += 5; return r.V; "
        "end Main; end C;")), 106);
}

// T14 — an OVERLOAD SET. §23.4: "Indexers may be overloaded by parameter type
// list." The virtual `[i: int]` overload must dispatch while the non-virtual
// `[k: string]` overload stays static — the vtable slots are keyed by index
// signature (#1080), so selecting the wrong one would show up here.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
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
        "  b[0] += 1; b[\"k\"] += 1; ";
    BEZEL_EXPECT_EQ(run(runAot(prog + "return d._s; end Main; end C;")), 203);
    // > 255, so the observable moves to stdout (the exit status is
    // WEXITSTATUS-masked to 8 bits).
    BEZEL_EXPECT_TRUE(runAotOut(
        prog + "Console.WriteLine(d._t); return 0; end Main; end C;") == "506");
}

// ---------------------------------------------------------------------------
// DISPATCH-CHAIN AND CONTROL-FLOW AXES
// ---------------------------------------------------------------------------

// T15 — a three-level chain B -> D -> E: the access must reach the MOST-derived
// override, not merely "one level down". E: get 300, +1 = 301, set stores 304.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
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
        prog + "using var e := new E(); Console.WriteLine(e.Bump()); "
               "return 0; end Main; end C;") == "304");
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var d := new D(); return d.Bump(); end Main; end C;")),
        203);
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var b := new B(); return b.Bump(); end Main; end C;")),
        102);
}

// T16 — a `virtual` indexer that the derived class does NOT override: the slot
// is filled with the base's own accessors, so dispatch and static binding must
// agree at 102. This is the shape a naive "if there is a slot, the answer is
// the derived body" fix would break.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           virtual_without_override_reaches_base_body_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") +
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Bump(): int "
        "    this[0] += 1; "
        "    return _s; "
        "  end Bump; "
        "end B; "
        "class D: B end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Bump(); "
        "end Main; end C;")), 102);
}

// T17 — a THROW out of the dispatched accessor (§31.1/§31.2). Both new branches
// call `emitPendingCheck` after each dispatched call, so the exception reaches
// the enclosing handler and NO store happens on that edge; the normal path in
// the same program still stores the dispatched result.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
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
        decls + "  try b[9] += 1; catch ArgumentException => return d._s; end; "
                "  return 250; end Main; end C;")), 0);
    // The SETTER throws: nothing is stored.
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "  try b[0] += 5000; catch ArgumentException => return d._s; "
                "  end; return 250; end Main; end C;")), 0);
    // The same accessors on the normal path still dispatch and store.
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "  b[0] += 1; return d._s; end Main; end C;")), 203);
}

// T18 — a `string`-typed element through the class-vtable branch. The integer
// cases above cannot observe the refcount discipline; this one runs the
// concatenating desugaring (`emitCompoundOp` -> `emitStringConcat`) through the
// dispatched accessors and pins the VALUE. D's getter appends "g", its setter
// appends "s": "b" -> "bg" -> "bgx" -> stored "bgxs".
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           string_element_dispatches_override_4_6) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public _s: string := \"b\"; "
        "  public virtual property [i: int]: string "
        "    get => _s + \"G\"; "
        "    set => do _s := value + \"S\"; end; "
        "  end; "
        "  public method Bump(): string "
        "    this[0] += \"x\"; "
        "    return _s; "
        "  end Bump; "
        "end B; "
        "class D: B "
        "  public override property [i: int]: string "
        "    get => _s + \"g\"; "
        "    set => do _s := value + \"s\"; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); Console.WriteLine(d.Bump()); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bgxs") != std::string::npos);
    // The statically-bound answer, which must NOT appear.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bGxS") == std::string::npos);
}

// T19 — a `char` element (§5.2/§6.6 #1961): the compound runs at int width and
// is narrowed back through the code-point check. D's getter yields 'a' (97),
// +1 = 98, D's setter stores 'b'. Asserted as an int so the observable is a
// number, per the harness note on char-typed observables.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
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
        "  using var d := new D(); var b: B := d; b[0] += 1; "
        "  return int(d._s); "
        "end Main; end C;")), 98);   // 'b'
}

// T20 — a GENERIC declaring class. The vtable branch reads `pd->type` and
// `pd->indexerParams` from the AST, which in a specialization must still lower
// against the active substitution.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           generic_declaring_class_dispatches_override_14_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B<T> "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Bump(): int "
        "    this[0] += 1; "
        "    return _s; "
        "  end Bump; "
        "end B; "
        "class D<T>: B<T> "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D<int>(); return d.Bump(); "
        "end Main; end C;")), 203);
}

// T21 — a CONSTRUCTOR and a DESTRUCTOR body. §22.7 admits an observation of
// `this` during construction in a `sealed`, non-abstract class, and the
// destructor runs on a fully-constructed object.
//
// ⚠ HONEST LABEL: this case is an ACCEPT CONTROL, not a fail-without case, and
// it passes with and without the fix. Inside a constructor or destructor body
// `this`'s static type IS the runtime type, so static binding and dynamic
// dispatch necessarily agree — no shape of this test could observe the defect.
// It earns its place because post-fix these bodies DO take the new vtable
// branch (a `Class` receiver, target not `base`), so it is the guard that the
// new branch does not break the two body kinds whose §22.7 posture is special.
// Constructor: 0 -> get 200 -> 201 -> stored 203.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
           constructor_and_destructor_bodies_22_7) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B; "
        "sealed class D: B "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "  public constructor() "
        "    this[0] += 1; "
        "  end constructor; "
        "  destructor "
        "    this[0] += 1; "
        "    Console.WriteLine(_s); "
        "  end destructor; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); Console.WriteLine(d._s); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // Constructor stored 203; the destructor then reads 403, +1 = 404, stores
    // 406. Both dispatched (statically bound would give 102 then 205).
    BEZEL_EXPECT_TRUE(r.stdoutText.find("203") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("406") != std::string::npos);
}

// T22 — a RECORD reached through an INTERFACE-typed variable: the itable branch
// can land on a record-backed implementor. §9 value semantics mean the write
// lands in the BOX, not in `r`, so both halves are asserted: reading back
// through the interface sees the stored value (get 200, +1 = 201, stored 203,
// read back 403) while the original record is untouched.
BEZEL_TEST(codegen_indexer_compound_virtual_2836,
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
        "  var r := new R(); var q: IBox := r; q[0] += 1; ";
    // > 255, so the observable moves to stdout.
    BEZEL_EXPECT_TRUE(runAotOut(
        prog + "Console.WriteLine(q[0]); return 0; end Main; end C;") == "403");
    BEZEL_EXPECT_EQ(run(runAot(prog + "return r.V; end Main; end C;")), 0);
}
