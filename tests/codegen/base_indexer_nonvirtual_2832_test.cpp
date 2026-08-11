// Spec-derived regression test for issue #2832.
//
// §12.3 (`base.Member`) / §23.4 (Indexers) / §12.5 (Abstract classes and
// members) / §7 (nullability), v0.240.
//
// §12.3 states, verbatim:
//
//     "Inside a derived member, `base.Name(...)` calls the base class's
//      implementation, bypassing virtual dispatch. `base` is legal only inside
//      instance members of a derived class."
//
// and §23.4, verbatim:
//
//     "Indexers may be overloaded by parameter type list. They participate in
//      `virtual`/`override`/`sealed`/`abstract` like methods."
//
// The spec never spells `base[i]` literally — §12.3 is written for
// `base.Name(...)`. Reading the two sentences together is what settles it: an
// indexer participates in virtual dispatch "like methods", and `base.` is the
// spelling that bypasses that dispatch, so `base[i]` denotes the base class's
// own indexer implementation, non-virtually. That reading is recorded on the
// fix commit as the interpretation this change commits to (the spec did not
// state it outright), and it is the ONLY reading consistent with the two
// named forms `base.P` / `base.M()`, both of which already bypass.
//
// SYMPTOM (the issue's own repro, measured on 6eaaecf): `base[i]` on a
// `virtual` base indexer overridden in the derived class compiled with NO
// diagnostic and returned the OVERRIDE's value — 202 where 102 is correct — on
// the one construct whose entire purpose is to bypass the override.
//
// TWO COOPERATING ROOT CAUSES, both fixed here:
//   1. `Emitter::indexReceiverDecl` (src/codegen/LlvmBackend_Assign.cpp)
//      answered `base` with the DERIVED type (its #747 R42 branch recovers
//      "which class's body is this", which is the right answer for its OTHER
//      caller — the inherited-generic-base call path in LlvmBackend_Calls2.cpp
//      — but not for an indexer access). `selectIndexer` was therefore handed
//      the derived type and picked its override. The fix routes indexer
//      accesses through the new `indexAccessReceiverDecl`, which takes the
//      §12.3 step to the class base.
//   2. That step alone is NOT sufficient: the base indexer's PropertySymbol
//      shares its vtable slot with the derived override, so the #1080
//      vtable-dispatch branches in `lowerIndexerGet` / `lowerIndexerSet` still
//      ran the override. They now decline a `base` receiver, exactly as
//      `lowerVirtualPropertyGet` / `-Set` already decline `base.P` and
//      `emitMethodCall`'s `baseQualified` declines `base.M()`.
//
// Sema half: `base[i]` was never TYPED (`base` carries no exprType, so the
// whole IndexExpr typing block was skipped — #2795's finding for `this[i]`).
// #2795 deliberately left it that way because typing it alone would only widen
// the set of spellings reaching the miscompile above. With the miscompile
// fixed, `src/sema/ExprTypecheck_Exprs.cpp` now resolves the receiver to the
// CLASS BASE, which gives `base[i]` its result type, the #2530 PropertySymbol,
// the #2543 write-only gate and the #2619 §7 index check.
//
// Rejections this typing newly makes possible are asserted on diagnostic TEXT,
// never the coarse `rejects()` helper (runAot's WIFSIGNALED path synthesizes an
// Error for a runtime SIGSEGV too).
//
// Related: #2795 (the `this[i]` half), #2833 (the lambda/async receiver
// recovery, companion commit — T8 needs both), #1080 (the vtable branches),
// #874 (the abstract-`base` method rejection this mirrors), #2837 (the
// cross-module virtual shape this change REJECTS rather than miscompiles),
// #2838 (`base.P` on an abstract property, the named twin, still silent).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAot;
using bezel::test::runAotOut;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

// True iff some reported diagnostic contains `sub`.
bool hasDiag(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The shared pair: a read-write `virtual` indexer on `B`, overridden by `D`,
// over a protected field so a read, a `:=` write and a `+=` compound are all
// observable and can tell B's accessors from D's.
constexpr const char* kBD =
    "class B "
    "  protected _s: int := 0; "
    "  public virtual property [i: int]: int "
    "    get => i + 100 + _s; "
    "    set => do _s := value + 1; end; "
    "  end; "
    "end B; "
    "class D: B "
    "  public override property [i: int]: int "
    "    get => i + 200 + _s; "
    "    set => do _s := value + 2; end; "
    "  end; ";

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO. Pre-fix: 202. Post-fix: 102 (B's `1 + 100`, then +1).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           base_index_read_runs_the_base_implementation_12_3) {
    auto r = runAot(
        std::string("module M; ") + kBD +
        "  public method ViaBase(i: int): int "
        "    var x := base[i]; "
        "    return x + 1; "
        "  end ViaBase; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.ViaBase(1); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 102);
}

// #2832 ACCEPT CONTROL / §12.1 — `this[i]` must STILL dispatch VIRTUALLY, and
// so must an ordinary typed receiver. This is the load-bearing control: a fix
// that made the vtable branch decline too widely would break every virtual
// indexer read. All three spellings see D's override on a D object.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           this_index_and_typed_receiver_stay_virtual_12_1) {
    auto r = runAotOut(
        std::string("module M; ") + kBD +
        "  public method ViaThis(i: int): int => this[i]; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); "
        "  var bref: B := d; "
        "  Console.WriteLine(d.ViaThis(1) * 1000000 + bref[1] * 1000 + d[1]); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r == "201201201");
}

// #2832 / §12.3 — the WRITE half. `base[0] := 4` must run B's setter
// (`_s := 4 + 1`), not D's (`_s := 4 + 2`). Pre-fix the vtable setter branch
// dispatched to D and `_s` came out 6.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           base_index_write_runs_the_base_setter_12_3) {
    auto r = runAot(
        std::string("module M; ") + kBD +
        "  public method W(v: int): int "
        "    base[0] := v; "
        "    return _s; "
        "  end W; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.W(4); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 5);
}

// #2832 / §12.3 — the COMPOUND half (`lowerIndexerCompound`, the third caller
// of the receiver resolution). Reads B's getter (0 + 100), adds 5, writes
// through B's setter (`_s := 105 + 1`). Pre-fix it read and wrote D's.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           base_index_compound_runs_the_base_accessors_12_3) {
    auto r = runAot(
        std::string("module M; ") + kBD +
        "  public method Bump(): int "
        "    base[0] += 5; "
        "    return _s; "
        "  end Bump; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Bump(); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 106);
}

// #2832 / §23.4 — a TWO-PARAMETER indexer ("Indexers may be overloaded by
// parameter type list"): the §12.3 base step must be arity-independent.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           base_index_two_parameters_23_4) {
    auto r = runAotOut(
        "module M; "
        "class B2 "
        "  public virtual property [r: int, c: int]: int => r * 10 + c + 1000; "
        "end B2; "
        "class D2: B2 "
        "  public override property [r: int, c: int]: int "
        "    => r * 10 + c + 2000; "
        "  public method ViaBase(): int => base[2, 3]; "
        "end D2; "
        "class C public static method Main(): int "
        "  using var d := new D2(); Console.WriteLine(d.ViaBase()); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r == "1023");
}

// #2832 ACCEPT CONTROL / §12.3 — a NON-VIRTUAL base indexer (no vtable slot,
// so the static accessor-body path was always taken). `base[k]` and `this[k]`
// must agree, and both must reach B's body. This is the "the fix did not
// change what already worked" control, on a STRING-typed result.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           nonvirtual_base_indexer_control_unchanged_12_3) {
    auto r = runAotOut(
        "module M; "
        "class B3 public property [k: string]: string => \"B:\" + k; end B3; "
        "class D3: B3 "
        "  public method ViaBase(): string => base[\"x\"]; "
        "  public method ViaThis(): string => this[\"y\"]; "
        "end D3; "
        "class C public static method Main(): int "
        "  using var d := new D3(); "
        "  Console.WriteLine(d.ViaBase() + d.ViaThis()); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r == "B:xB:y");
}

// #2832 (sema half) / §23.4 — the RESULT TYPE. `base[i].ToString()` needs the
// IndexExpr's own type; untyped it degraded to the #2434 zero-initialized
// value. This asserts BOTH halves at once: it must format the BASE's number.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           base_index_result_type_supports_member_access_23_4) {
    auto r = runAotOut(
        std::string("module M; ") + kBD +
        "  public method S(): string => base[1].ToString(); "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); Console.WriteLine(d.S()); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r == "101");
}

// #2832 + #2833 / §12.3 + §27.3 — `base[i]` inside a LAMBDA body. Needs BOTH
// halves: the receiver recovery (#2833's fallback, which resolves through the
// PropertySymbol sema attached against the BASE — which only exists because of
// this commit's sema half) AND the non-virtual dispatch. 101, not 201, and not
// the 0 a dropped receiver would give.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           base_index_inside_a_lambda_body_27_3) {
    auto r = runAot(
        std::string("module M; ") + kBD +
        "  public method ViaBase(i: int): int "
        "    var f := lambda(n: int) => base[n]; "
        "    return f(i); "
        "  end ViaBase; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.ViaBase(1); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 101);
}

// #2832 + #2833 / §12.3 + §30.1 — the same through an `async` body.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           base_index_inside_an_async_body_30_1) {
    auto r = runAot(
        std::string("module M; ") + kBD +
        "  public async method A(): Task<int> return base[1]; end A; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.A().Result; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 101);
}

// ---------------------------------------------------------------------------
// REJECTIONS. Each asserts on diagnostic TEXT.
// ---------------------------------------------------------------------------

// #2832 / §12.3 — "`base` is legal only inside instance members of a DERIVED
// class". A class with no class base has `Object` as its base and `Object`
// declares no indexer, so `base[i]` names nothing. Pre-fix it compiled and
// silently read the class's OWN indexer (measured: 301). Mirrors the named
// form's #1106 defect-6 rejection.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           base_index_with_no_class_base_is_rejected_12_3) {
    auto r = runAot(
        "module M; "
        "class D "
        "  public property [i: int]: int => i + 300; "
        "  public method ViaBase(i: int): int => base[i]; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.ViaBase(1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "does not name a base-class indexer"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "has no class base"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "(\xC2\xA7""12.3/\xC2\xA7""23.4)"));
}

// #2832 / §12.3 + §12.5 — an ABSTRACT base indexer has no implementation for
// `base` to reach. Pre-fix this ran the DERIVED override (the #2832
// miscompile); with the receiver step it would instead degrade to a zero/null
// value, since the static accessor-body path finds no body. Reject instead,
// mirroring #874's "cannot call abstract base method `M` via `base` — it has
// no implementation (§12.3/§12.5)".
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           abstract_base_indexer_via_base_is_rejected_12_5) {
    auto r = runAot(
        "module M; "
        "abstract class AB public abstract property [i: int]: int; end AB; "
        "class AD: AB "
        "  public override property [i: int]: int => i + 200; "
        "  public method ViaBase(i: int): int => base[i]; "
        "end AD; "
        "class C public static method Main(): int "
        "  using var d := new AD(); return d.ViaBase(1); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasDiag(r, "abstract base indexer"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "no implementation"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "(\xC2\xA7""12.3/\xC2\xA7""12.5)"));
}

// #2832 (sema half) / §7 — typing `base[i]` also brings the #2619 §7
// index-argument check to it. An un-narrowed `int?` index used to compile with
// NO diagnostic and lower the null to 0.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           base_index_rejects_unnarrowed_nullable_index_7) {
    auto r = runAot(
        std::string("module M; ") + kBD +
        "  private static method NI(): int? => null; "
        "  public method Bad(): int => base[NI()]; "
        "end D; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(
        hasDiag(r, "is nullable but the target type is non-nullable"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "index argument to `B[i]`"));
}

// #2832 ACCEPT CONTROL / §12.3 — the STATIC rejection stays the ONLY
// diagnostic there. `base`/`this` in a static member is already rejected by
// name; the new typing must not add a second, cascading message.
BEZEL_TEST(codegen_base_indexer_nonvirtual_2832,
           base_index_in_a_static_method_keeps_its_rejection_12_3) {
    auto r = runAot(
        std::string("module M; ") + kBD +
        "  public static method S(): int => base[1]; "
        "end D; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(
        hasDiag(r, "`base` is not available in a static method"));
    BEZEL_EXPECT_FALSE(hasDiag(r, "does not name a base-class indexer"));
    BEZEL_EXPECT_FALSE(hasDiag(r, "abstract base indexer"));
}
