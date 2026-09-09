// Spec-derived regression test for issue #2858.
//
// §17.6 (The increment and decrement operators) / §12.1 (Members are
// non-virtual by default) / §12.3 (`base.Member`) / §11.2 (Interface
// declaration) / §23.1 (Auto-properties), v0.240.
//
// §17.6 states, verbatim:
//
//     "The operand must be an assignable lvalue (a local variable, a writable
//      field, an indexer slot, or a property with a setter — the same storage
//      categories §22.3's `ref`/`out` arguments reuse)."
//
//     "For built-in numeric types, `x++` is equivalent to `x := x + 1` (and
//      likewise `x--` to `x := x - 1`), with the same overflow semantics as `+`
//      and `-`."
//
// and §12.1, verbatim:
//
//     "A method, property, or indexer is non-virtual unless marked `virtual`.
//      To override a virtual member in a derived class, mark the matching
//      member `override`."
//
// THE DEFECT. `P++` on a `virtual`/`override` property is a THIRD lowering path,
// distinct from `P := v` (#2849) and from `a[i]++` (#2848). `lowerStmt`'s
// `ast::IncrementStmt` arm reached the setter through `lowerPropertySetValue`
// (statically bound) or, for an auto-property, through `lvalueAddr` (a direct
// store into the DECLARING class's backing field) — never through
// `lowerVirtualPropertySet`, which is what the same function's AssignmentStmt
// arm calls for both `:=` and `OP=`. The READ half went through `lowerExpr` and
// therefore DID dispatch, so the two halves of one statement disagreed.
//
// MEASURED on dafe6d9 (i.e. with #2849 and #2848 already landed) with `B`'s
// accessors at +100/+1 and `D`'s at +200/+2, receiver always a `D`:
//
//     spelling                          measured   correct
//     P++    (bare)                       202        203     virtual GET, static SET
//     this.P++                            202        203     same
//     b.P++  (base-typed reference)       202        203     same
//     P++    on an `abstract` property      0        203     DROPPED
//     A++    on a virtual AUTO-property      0          1     DROPPED
//     q.P++  through an interface           1        203     WRONG, and not even
//                                                            the static answer
//     Dm++   on a `decimal` property     IR verification failure (compile abort)
//
// 202 is the diagnostic value: the getter dispatched (D's 200, +1 = 201) and the
// setter did not (B's `value + 1` = 202).
//
// THE FIX adds `lowerVirtualPropertyIncrDecr`, called from the IncrementStmt arm
// BEFORE the `lvalueAddr` probe (so it also preempts the auto-property direct
// store) and gated on `!opMethod` (§17.6, verbatim: "For a value-type operand
// whose type T overloads `operator ++`, the statement `x++` is equivalent to
// `x := T.operator++(x)`" — that shape keeps the operator path). It declines,
// before emitting anything, for a non-property target, an indexer, `base.P`,
// and a property with no vtable/itable slot.
//
// Every case asserts a CORRECT VALUE, not a clean compile: the defect compiled
// cleanly and produced the wrong number (or nothing at all). Constants are
// < 256 where the observable is the exit status (WEXITSTATUS-masked to 8 bits).
//
// Related: #2849 (the bare-name `:=` / `OP=` write, whose fix does not reach
// this path), #2848 (the same arm's INDEXER branch), #2836, #2833, #2832, #481,
// #2859 (a NULLABLE-typed virtual property still cannot be written on any
// spelling — out of scope here and untouched).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

// The shared virtual/override pair. `B`'s accessors are +100/+1, `D`'s are
// +200/+2, so `P++` on a `D` yields
//   * 203 when BOTH halves dispatch to D (correct),
//   * 202 when only the getter dispatched (the defect),
//   * 102 when both bind statically to B,
// three values that cannot be confused for one another.
constexpr const char* kPair =
    "class B "
    "  public _s: int := 0; "
    "  public virtual property P: int "
    "    get => _s + 100; "
    "    set => do _s := value + 1; end; "
    "  end; "
    "  public method IncBare(): int   P++;       return _s; end IncBare; "
    "  public method IncThis(): int   this.P++;  return _s; end IncThis; "
    "  public method DecBare(): int   P--;       return _s; end DecBare; "
    "end B; "
    "class D: B "
    "  public override property P: int "
    "    get => _s + 200; "
    "    set => do _s := value + 2; end; "
    "  end; "
    "end D; ";

}  // namespace

// ---------------------------------------------------------------------------
// T1 — the three spellings from the issue's table. §17.6 / §12.1.
// Pre-fix all three: 202. Post-fix all three: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_property_increment_virtual_2858,
           bare_this_and_typed_receiver_dispatch_17_6) {
    const std::string prog = std::string("module M; ") + kPair +
        "class C public static method Main(): int ";
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var d := new D(); return d.IncBare(); end Main; end C;")),
        203);
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var d := new D(); return d.IncThis(); end Main; end C;")),
        203);
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var d := new D(); var b: B := d; b.P++; return d._s; "
               "end Main; end C;")), 203);
}

// `--` is the same desugaring with the operator changed (§17.6: "and likewise
// `x--` to `x := x - 1`"): D's getter gives 200, 200-1 = 199, D's setter stores
// 201. Pre-fix: 200 (B's setter stored 199+1).
BEZEL_TEST(codegen_property_increment_virtual_2858,
           decrement_dispatches_override_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); return d.DecBare(); "
        "end Main; end C;")), 201);
}

// ---------------------------------------------------------------------------
// T2 — an ABSTRACT read-write property incremented inside the abstract base's
// own concrete method. §12.5: "`abstract` on a member declares it without a
// body", so the static path had no setter body to inline and the statement was
// DROPPED. Pre-fix: 0. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_property_increment_virtual_2858,
           abstract_property_increment_12_5) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "abstract class A "
        "  public _s: int := 0; "
        "  public abstract property P: int get; set; end; "
        "  public method Inc(): int  P++;  return _s; end Inc; "
        "end A; "
        "class D: A "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T3 — a virtual AUTO-property (§23.1's `property A: int := 0;` form). This is
// the shape that made the fix's PLACEMENT load-bearing: `lvalueAddr` succeeds
// for an auto-property and resolves to the DECLARING class's backing field, so
// the direct store landed in `B`'s field while every read went through the
// dispatched getter to `D`'s. Pre-fix: 0 (the write was invisible).
// Post-fix: 1.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_property_increment_virtual_2858,
           virtual_auto_property_increment_23_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public virtual property A: int := 0; "
        "  public method Inc(): int  A++;  return A; end Inc; "
        "end B; "
        "class D: B "
        "  public override property A: int := 0; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc(); "
        "end Main; end C;")), 1);
}

// ---------------------------------------------------------------------------
// T4 — an INTERFACE-typed receiver (§11.2). Pre-fix this gave 1 — not even the
// statically-bound answer, since `computedSetterOf` returns null for a bodiless
// interface PropertyDecl and the whole accessor chain fell through.
// Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_property_increment_virtual_2858,
           interface_typed_receiver_dispatches_itable_11_2) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "interface IP property P: int; end IP; "
        "class D: IP "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); var q: IP := d; q.P++; return d._s; "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T5 — inside a LAMBDA body (§27.3) and T6 — inside an ASYNC body (§30.1).
// Pre-fix both: 202. Post-fix both: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_property_increment_virtual_2858,
           increment_inside_lambda_body_27_3) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Inc(): int "
        "    var f := lambda() => do P++; end; "
        "    f(); "
        "    return _s; "
        "  end Inc; "
        "end B; "
        "class D: B "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc(); "
        "end Main; end C;")), 203);
}

BEZEL_TEST(codegen_property_increment_virtual_2858,
           increment_inside_async_body_30_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public async method Inc(): Task<int> "
        "    P++; "
        "    return _s; "
        "  end Inc; "
        "end B; "
        "class D: B "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc().Result; "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T7 — a `sealed override` reached through the BASE's method (§12.4: "`sealed`
// on an override prevents further overriding"). Sealing ends the chain but does
// NOT remove the slot. Pre-fix: 202. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_property_increment_virtual_2858,
           sealed_override_still_dispatches_12_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class DS: B "
        "  public sealed override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end DS; "
        "class C public static method Main(): int "
        "  using var ds := new DS(); return ds.IncBare(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T8 — a three-level chain B -> D -> E: the MOST-DERIVED override. E's getter
// is +300 and its setter +3, so `P++` inside B's method on an `E` is
// 300 -> 301 -> 304. Pre-fix: 302 (E's getter dispatched, B's setter did not) —
// the clearest single demonstration that only the setter half was static.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_property_increment_virtual_2858,
           most_derived_override_in_a_chain_12_1) {
    const std::string prog =
        std::string("module M; ") + kPair +
        "class E: D "
        "  public override property P: int "
        "    get => _s + 300; "
        "    set => do _s := value + 3; end; "
        "  end; "
        "end E; "
        "class C public static method Main(): int ";
    // > 255, so the observable moves to stdout.
    BEZEL_EXPECT_TRUE(runAotOut(
        prog + "using var e := new E(); Console.WriteLine(e.IncBare()); "
               "return 0; end Main; end C;") == "304");
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var d := new D(); return d.IncBare(); end Main; end C;")),
        203);
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var b := new B(); return b.IncBare(); end Main; end C;")),
        102);
}

// ---------------------------------------------------------------------------
// T9 — a GENERIC declaring class. Pre-fix: 202. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_property_increment_virtual_2858,
           generic_declaring_class_dispatches_14_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B<T> "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Inc(): int  P++;  return _s; end Inc; "
        "end B; "
        "class D<T>: B<T> "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D<int>(); return d.Inc(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// VALUE-KIND AXES — the `1` literal is synthesized at the getter's LOWERED
// type, so each numeric family is its own case.
// ---------------------------------------------------------------------------

// T10 — a `double` property (ConstantFP). Pre-fix: 202. Post-fix: 203.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           double_property_dispatches_6_3) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _f: double := 0.0; "
        "  public virtual property F: double "
        "    get => _f + 100.0; "
        "    set => do _f := value + 1.0; end; "
        "  end; "
        "  public method Inc(): int  F++;  return int(_f); end Inc; "
        "end B; "
        "class D: B "
        "  public override property F: double "
        "    get => _f + 200.0; "
        "    set => do _f := value + 2.0; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc(); "
        "end Main; end C;")), 203);
}

// T11 — a `decimal` property. §6.4: `decimal` is a STRUCT-typed primitive, so
// `emitCompoundOp` would feed it to the integer overflow intrinsic; the fix
// routes it through the decimal arithmetic helper instead, as this same
// statement handler's `lvalueAddr` path already does for a decimal local.
//
// ⚠ Pre-fix this shape did not merely produce a wrong value — it ABORTED THE
// COMPILE: "Call parameter type does not match function signature! i33 1 ...
// call { %bz.decimal, i1 } @llvm.sadd.with.overflow.s_bz.decimals". So this
// case turns a compile failure into a working program. Post-fix: 203.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           decimal_property_dispatches_6_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _d: decimal := 0m; "
        "  public virtual property Dm: decimal "
        "    get => _d + 100m; "
        "    set => do _d := value + 1m; end; "
        "  end; "
        "  public method Inc(): int  Dm++;  return int(_d); end Inc; "
        "end B; "
        "class D: B "
        "  public override property Dm: decimal "
        "    get => _d + 200m; "
        "    set => do _d := value + 2m; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc(); "
        "end Main; end C;")), 203);
}

// T12 — a `char` property (§5.2 / §6.6 #1961): the read-modify-write runs at
// int width and is narrowed back through the code-point check. D's getter
// yields 'a' (97), +1 = 98, and D's setter additionally records +100 in a
// witness field so the SETTER half is observable independently of the getter's.
// Dispatched: 98 + 100 = 198. Statically bound: B's getter reads 'A' (65),
// +1 = 66, B's setter records 0 -> 66. Pre-fix: 66. Post-fix: 198.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           char_property_dispatches_5_2) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _c: char := 'A'; "
        "  public _w: int := 0; "
        "  public virtual property Ch: char "
        "    get => _c; "
        "    set => do _c := value; _w := 0; end; "
        "  end; "
        "  public method Inc(): int  Ch++;  return int(_c) + _w; end Inc; "
        "end B; "
        "class D: B "
        "  public override property Ch: char "
        "    get => 'a'; "
        "    set => do _c := value; _w := 100; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Inc(); "
        "end Main; end C;")), 198);
}

// ---------------------------------------------------------------------------
// ACCEPT CONTROLS — shapes the fix must NOT change. This branch runs BEFORE
// `lvalueAddr` for every IncrementStmt in the program, so it is the one place
// where a too-wide predicate would break unrelated working code. Each of these
// is asserted at the SAME value before and after.
// ---------------------------------------------------------------------------

// A plain LOCAL increment — the overwhelmingly common shape, and the one that
// reaches this arm first. 6 before and after.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           plain_local_increment_unchanged_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; class C public static method Main(): int "
        "  var n := 5; n++; return n; "
        "end Main; end C;")), 6);
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "A user-defined `operator ++` on a record-typed FIELD ... The
// new branch is gated on `!opMethod` so this keeps the operator path. 10
// before and after." §25.1/§17.6 retire the `operator ++`/`operator --`
// overload mechanism outright at v0.295 — `Counter`'s declaration is now
// rejected on its own, and `h.C++` is independently rejected at the use
// site regardless of the (now-illegal) declaration.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           user_operator_increment_declaration_rejected_spec_25_1_issue_3978) {
    auto r = runAot(
        "module M; "
        "record Counter "
        "  public V: int := 0; "
        "  public constructor(v: int)  V := v;  end constructor; "
        "  public static method operator ++(c: Counter): Counter "
        "    return new Counter(c.V + 10); "
        "  end; "
        "end Counter; "
        "class H  public C: Counter := new Counter(0);  end H; "
        "class C public static method Main(): int "
        "  using var h := new H(); h.C++; return h.C.V; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    bool citedDecl = false, citedUse = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("operator `++` may not be overloaded (§25.1)") !=
            std::string::npos)
            citedDecl = true;
        if (d.message.find("`++` is not defined on record type `Counter`") !=
            std::string::npos)
            citedUse = true;
    }
    BEZEL_EXPECT_TRUE(citedDecl);
    BEZEL_EXPECT_TRUE(citedUse);
}

// A NON-VIRTUAL property and a NON-VIRTUAL auto-property have no vtable slot,
// so the new branch declines and both keep their existing lowering. 102 and 1
// before and after.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           non_virtual_property_stays_static_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class N "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Inc(): int  P++;  return _s; end Inc; "
        "end N; "
        "class C public static method Main(): int "
        "  using var n := new N(); return n.Inc(); "
        "end Main; end C;")), 102);
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class N "
        "  public property A: int := 0; "
        "  public method Inc(): int  A++;  return A; end Inc; "
        "end N; "
        "class C public static method Main(): int "
        "  using var n := new N(); return n.Inc(); "
        "end Main; end C;")), 1);
}

// A STATIC property. §12.1, verbatim: "Fields, constructors, destructors, and
// static members may not be virtual", so buildVtable never gives one a slot and
// the new branch declines it even though it is otherwise a property lvalue.
// 102 before and after.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           static_property_increment_unchanged_22_12) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class H "
        "  public static _q: int := 0; "
        "  public static property SP: int "
        "    get => _q + 100; "
        "    set => do _q := value + 1; end; "
        "  end; "
        "  public static method Inc(): int  SP++;  return _q; end Inc; "
        "end H; "
        "class C public static method Main(): int "
        "  return H.Inc(); "
        "end Main; end C;")), 102);
}

// `base.P++` must run the BASE's implementation. §12.3, verbatim: "Inside a
// derived member, `base.Name(...)` calls the base class's implementation,
// bypassing virtual dispatch." The base property shares its vtable slot with
// the derived override, so a dispatched `base.P` would run the override. This
// one was ALREADY correct before the fix, which is exactly why the decline is
// checked before the getter is lowered rather than after. 102 before and after.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           base_qualified_increment_stays_non_virtual_12_3) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class DB: B "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "  public method IncBase(): int  base.P++;  return _s; end IncBase; "
        "end DB; "
        "class C public static method Main(): int "
        "  using var d := new DB(); return d.IncBase(); "
        "end Main; end C;")), 102);
}

// A `virtual` property the derived class does NOT override: the slot holds the
// base's own accessors, so dispatch and static binding must agree at 102. This
// is the shape a naive "if there is a slot, run the derived body" fix would
// break. 102 before and after.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           virtual_without_override_reaches_base_body_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class N: B end N; "
        "class C public static method Main(): int "
        "  using var n := new N(); var b: B := n; b.P++; return n._s; "
        "end Main; end C;")), 102);
}

// The INDEXER spelling still goes to `lowerIndexerIncrDecr` (#2848), not here —
// the new branch declines an indexer PropertySymbol outright. 203 before (with
// #2848) and after.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           indexer_increment_still_routes_to_2848_23_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method Inc(): int  this[0]++;  return _s; end Inc; "
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

// A throw out of the dispatched accessor (§31.1/§31.2): `lowerVirtualPropertySet`
// calls `emitPendingCheck` after the dispatched call, so the exception reaches
// the enclosing handler and NO store happens on that edge.
BEZEL_TEST(codegen_property_increment_virtual_2858,
           throw_from_dispatched_setter_31_1) {
    const std::string decls =
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B; "
        "class D: B "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do "
        "      if value > 250 then throw new ArgumentException(\"big\"); end; "
        "      _s := value + 2; "
        "    end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); var b: B := d; ";
    // Prime `_s` so the dispatched getter (200 + 100) exceeds the setter's
    // threshold: the setter throws and NOTHING is stored on that edge, so `_s`
    // is still the primed 100. (The statically-bound setter would not have
    // thrown at all — B's has no guard — so this also pins the dispatch.)
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "  d._s := 100; "
                "  try b.P++; catch ArgumentException => return d._s; end; "
                "  return 250; end Main; end C;")), 100);
    // The same accessors on the normal path still dispatch and store.
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "  b.P++; return d._s; end Main; end C;")), 203);
}
