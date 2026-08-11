// Codegen slice CG49 — block-form property setters (§22.9). A property with
// a `set => do … end` (or `set => expr`) accessor runs that body on
// `obj.P := v`, with the implicit `value` parameter (renamable via
// `set(name)`) bound to the assigned value. The getter side (CG33c) and
// auto-properties are unchanged. Observable via the int exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg49, block_setter_stores_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 42; return c.P; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg49, setter_transforms_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int "
        "    get => _x; set => do _x := value * 2; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 5; return c.P; "
        "  end Main; "
        "end C;"), 10);
}

BEZEL_TEST(codegen_cg49, renamed_setter_parameter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int "
        "    get => _x; set(v) => do _x := v + 1; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 7; return c.P; "
        "  end Main; "
        "end C;"), 8);
}

BEZEL_TEST(codegen_cg49, setter_via_implicit_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => do _x := value; end; end; "
        "  public method Init() P := 33; end Init; "
        "  public static method Main(): int "
        "    var c := new C(); c.Init(); return c.P; "
        "  end Main; "
        "end C;"), 33);
}

BEZEL_TEST(codegen_cg49, auto_property_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public property V: int; "
        "  public static method Main(): int "
        "    var c := new C(); c.V := 99; return c.V; "
        "  end Main; "
        "end C;"), 99);
}

// §22.9 (issue #248): compound assignment on a block-form property is a
// read-modify-write through the getter and setter, not a silent no-op.
BEZEL_TEST(codegen_cg49, compound_assign_runs_getter_and_setter) {
    // Doubling setter: `:= 10` stores _x=20; `+= 5` reads 20, +5 = 25, the
    // setter doubles → _x = 50.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int "
        "    get => _x; set => do _x := value * 2; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 10; c.P += 5; return c.P; "
        "  end Main; "
        "end C;"), 50);
}

BEZEL_TEST(codegen_cg49, compound_assign_identity_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 20; c.P -= 7; return c.P; "
        "  end Main; "
        "end C;"), 13);
}

BEZEL_TEST(codegen_cg49, compound_assign_times_equals) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 6; c.P *= 4; return c.P; "
        "  end Main; "
        "end C;"), 24);
}

// #739 / §21.7: a `return;` in a block-form setter ENDS the setter (it is a void
// accessor), it does NOT return from the enclosing method. The setter body is
// inlined at the assignment site, so the return is routed to an accessor-end
// block. Conditional-return validate-then-set must keep the prior value.
BEZEL_TEST(codegen_block_setter, conditional_return_ends_setter_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box "
        "  public _x: int := 0; "
        "  public property X: int "
        "    get => _x; "
        "    set => do if value < 0 then return; end; _x := value; end; "
        "  end X; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); "
        "  b.X := 5; b.X := -1; b.X := 8; "  // -1 rejected; X = 8
        "  return b.X; "
        "end Main; end C;"), 8);
}

// #739 / §21.7: an UNCONDITIONAL `return;` in a setter previously IR-failed
// (terminator mid-block). Now it cleanly ends the setter (a no-op setter here).
BEZEL_TEST(codegen_block_setter, unconditional_return_in_setter_no_irfail_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box "
        "  public _x: int := 7; "
        "  public property X: int get => _x; set => do return; end; end X; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); b.X := 99; "  // setter is a no-op
        "  return b.X; "  // still 7
        "end Main; end C;"), 7);
}

// ---------------------------------------------------------------------------
// #1236 / §23.3 / §23.5: `return;` inside a user-defined INDEXER setter body.
//
// An indexer setter body is inlined into the assignment site (like a regular
// property setter). Without the #1236 fix, `return;` fell through to the
// enclosing method's return machinery instead of the setter-end continuation:
//   Facet A (unconditional return): a second terminator was emitted into the
//     `using.cls.cont` BB → IR verification failure.
//   Facet B (conditional return): the enclosing `using var`'s disposal ran
//     early (inside the setter), freeing the object before the subsequent
//     read — the write happened but the object was freed, giving wrong value.
// Fix (LlvmBackend_Assign.cpp `lowerIndexerSet`): install `inlinedAccessorEnd_`
// before `lowerScopedBlock`, mirroring `lowerPropertySetValue` (§21.7 / #739).
// ---------------------------------------------------------------------------

// #1236 / §23.5: unconditional `return;` at the end of a block-form indexer
// setter body must NOT produce IR verification failure ("Terminator found in
// the middle of a basic block"). The write before `return;` must be committed.
BEZEL_TEST(codegen_block_setter, indexer_setter_unconditional_return_no_irfail_1236) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; return; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 42; "
        "    return c[0]; "
        "  end Main; end C;"), 42);
}

// #1236 / §23.5: conditional `return;` inside an indexer setter body — the
// write executed before `return;` must persist; the enclosing method's locals
// must NOT be disposed by the setter's `return;`.
BEZEL_TEST(codegen_block_setter, indexer_setter_conditional_return_write_persists_1236) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do "
        "      if value >= 0 then _d[i] := value; return; end; "
        "    end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 42; "
        "    return c[0]; "
        "  end Main; end C;"), 42);
}

// #1236 / §23.5: validate-then-reject: negative values are rejected by an
// early `return;`, positive values are stored — mirrors the §21.7 / #739
// pattern but for the indexer setter. Last non-negative write wins.
BEZEL_TEST(codegen_block_setter, indexer_setter_return_rejects_invalid_1236) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do if value < 0 then return; end; _d[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 5; c[0] := -1; c[0] := 8; "   // -1 rejected; slot = 8
        "    return c[0]; "
        "  end Main; end C;"), 8);
}

// #1236 / §23.5: `return;` in the `else` branch of an indexer setter — both
// the taken branch (else) and the fall-through both end at the setter-end
// continuation (not the enclosing method's return).
BEZEL_TEST(codegen_block_setter, indexer_setter_return_in_else_branch_1236) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do "
        "      if value < 0 then return; else _d[i] := value; return; end; "
        "    end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 42; "
        "    return c[0]; "
        "  end Main; end C;"), 42);
}

// #1236 REGRESSION / §23.5: a block-form indexer setter WITHOUT a `return;`
// (plain fall-through) still stores the value correctly (the fix must not
// perturb the normal non-returning case).
BEZEL_TEST(codegen_block_setter, indexer_setter_no_return_fallthrough_regression_1236) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 42; "
        "    return c[0]; "
        "  end Main; end C;"), 42);
}

// #1236 REGRESSION / §22.9: a regular (non-indexer) property setter with
// `return;` still works after the indexer-setter fix (the regular path is
// unaffected; this guards against accidental shared-state clobbering).
BEZEL_TEST(codegen_block_setter, regular_setter_return_regression_1236) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box "
        "  public _x: int := 0; "
        "  public property X: int "
        "    get => _x; "
        "    set => do if value < 0 then return; end; _x := value; end; "
        "  end X; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); "
        "  b.X := 5; b.X := -1; b.X := 8; "   // -1 rejected; X = 8
        "  return b.X; "
        "end Main; end C;"), 8);
}

// #1236 / §23.5: reading an indexer slot that was never written returns 0
// (the zero-initialized backing array).
BEZEL_TEST(codegen_block_setter, indexer_unwritten_slot_is_zero_1236) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; return; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 42; "
        "    return c[1]; "   // slot 1 never written → 0
        "  end Main; end C;"), 0);
}
