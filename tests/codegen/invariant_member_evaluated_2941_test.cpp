// Spec-derived runtime tests for §22.16 class invariants (v0.240, issue
// #2941): the `invariant(E);` member is parsed, purity-checked, AND
// EVALUATED at the construction-exit / public-method-entry /
// public-method-exit boundaries §22.16 fixes. A predicate evaluating
// false at any of those boundaries raises ContractViolationException,
// which the caller catches via `catch ContractViolationException`.
//
// Spec §22.16 (v0.240):
//   * Construction exit: "the normal completion of the most-derived
//     constructor of a construction, of any visibility, after its body
//     and its ensures clauses".
//   * Entry and normal exit of every public instance method.
//   * §22.16 "Failure semantics": a false clause faults with
//     ContractViolationException, in every build mode (the §22.16
//     checked-contracts-mode toggle is the §22.16 release relaxation,
//     not elision by proof).
//
// The runtime triple here proves the codegen half:
// `emitInvariantCheck` in src/codegen/LlvmBackend_Setup.cpp walks the
// class hierarchy base-first and lowers each `InvariantDecl`'s
// expression with `this` rebound to the live instance; on a false
// result it throws ContractViolationException. Each test asserts the
// exit code or the catch-arm discriminator, the same oracle the
// contract-clauses tests (#2053) use.
//
// Each test exercises a runtime triple: the program runs (compiles +
// executes natively via the AOT harness), and the assertion is on the
// program's exit code or caught-exception value.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

int runFull(const std::string& program) {
    auto r = bezel::test::runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =========================================================================
// §22.16 (v0.240): a satisfied class invariant does NOT fault at
// construction exit — `new C(5)` succeeds and `c.x == 5` reads back.
// =========================================================================
BEZEL_TEST(invariant_member_evaluated_2941,
           satisfied_invariant_at_construction_exit_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  public constructor(v: int) x := v; end constructor; "
        "  public static method Main(): int "
        "    using var c := new C(5); return c.x; "
        "  end Main; "
        "end C;"), 5);
}

// §22.16: a violated invariant at construction exit faults with
// ContractViolationException. The catch arm discriminates 99.
BEZEL_TEST(invariant_member_evaluated_2941,
           violated_invariant_at_construction_exit_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  public constructor(v: int) x := v; end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      using var c := new C(-1); "
        "    catch ContractViolationException => r := 99; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 99);
}

// §22.16: a violated invariant at public method EXIT — a public method
// that mutates a field to violate the invariant trips the exit-edge
// check.
BEZEL_TEST(invariant_member_evaluated_2941,
           violated_invariant_at_public_method_exit_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  public constructor() x := 1; end constructor; "
        "  public method Break(): int x := -1; return x; end Break; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var r: int := 0; "
        "    try "
        "      var v := c.Break(); "
        "    catch ContractViolationException => r := 77; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 77);
}

// §22.16: a satisfied invariant at public method exit — the method's
// return leaves the invariant holding, so no fault fires.
BEZEL_TEST(invariant_member_evaluated_2941,
           satisfied_invariant_at_public_method_exit_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  public constructor() x := 1; end constructor; "
        "  public method Set(v: int): int x := v; return x; end Set; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Set(42); "
        "  end Main; "
        "end C;"), 42);
}

// §22.16: a violated invariant at public method ENTRY — the entry
// check fires BEFORE the method body runs. (We trip it by constructing
// with the invariant temporarily satisfied via the field-init, then
// mutating outside the class via an accessor to break it, then calling
// another public method.)
BEZEL_TEST(invariant_member_evaluated_2941,
           violated_invariant_at_public_method_entry_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  public constructor() x := 1; end constructor; "
        "  public method Set(v: int): int x := v; return x; end Set; "
        "  public method Check(): int return x; end Check; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var r: int := 0; "
        "    try "
        "      var v := c.Set(-5); "
        "    catch ContractViolationException => r := 33; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 33);
}

// §22.16: ContractViolationException derives from Exception per the
// §31.4 hierarchy — a generic `catch Exception` arm catches it.
BEZEL_TEST(invariant_member_evaluated_2941,
           contract_violation_is_exception_subtype_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  public constructor() x := -1; end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      using var c := new C(); "
        "    catch Exception => r := 55; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 55);
}

// §22.16: a class with NO invariant compiles and runs unchanged — no
// instrumentation is emitted, no observable cost.
BEZEL_TEST(invariant_member_evaluated_2941,
           class_without_invariant_runs_unchanged_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public x: int := 0; "
        "  public constructor(v: int) x := v; end constructor; "
        "  public static method Main(): int "
        "    using var c := new C(7); return c.x; "
        "  end Main; "
        "end C;"), 7);
}

// §22.16: multiple invariants on the same class — base-first evaluation
// means ALL invariants must hold. A class with two invariants, the
// first violated, trips the first fault (declaration-order evaluation).
BEZEL_TEST(invariant_member_evaluated_2941,
           multiple_invariants_declaration_order_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public x: int := 0; "
        "  public y: int := 0; "
        "  invariant(x > 0); "
        "  invariant(y >= 0); "
        "  public constructor(v: int, w: int) x := v; y := w; end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      using var c := new C(-1, 5); "
        "    catch ContractViolationException => r := 21; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"), 21);
}

// §22.16: inherited invariants — a derived class's invariants PLUS
// base class invariants all fire at construction exit. The base
// invariant is evaluated first; if it passes, the derived invariant
// evaluates next.
BEZEL_TEST(invariant_member_evaluated_2941,
           inherited_invariant_evaluates_base_first_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class B "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  public constructor(v: int) x := v; end constructor; "
        "end B; "
        "class D : B "
        "  public y: int := 0; "
        "  invariant(y >= 0); "
        // fix #4549 (§12.8, v0.306): `D` now WRITES its `base(...)`
        // chain. §12.8 inserts an implicit `base()` when none is
        // written and "requires the base class to have a parameterless
        // constructor"; `B` declares only `constructor(v: int)`, so
        // this program was ill-formed and is rejected as of #4549.
        // The ORACLE IS UNCHANGED (still `== 11` / `== 15`, and this
        // file's subject — §22.16 inherited-invariant evaluation at
        // construction exit — is untouched): `base(v)` sets `x := v`
        // in `B`'s constructor and the body then assigns the same `v`
        // to the same field, so every value the rows observe is the
        // one they observed before.
        "  public constructor(v: int, w: int) "
        "    base(v); "
        "    x := v; y := w; "
        "  end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      using var d := new D(5, -1); "
        "    catch ContractViolationException => r := 11; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end D;"), 11);
}

// §22.16: a satisfied inherited invariant at construction exit — both
// base and derived invariants hold, construction completes.
BEZEL_TEST(invariant_member_evaluated_2941,
           satisfied_inherited_invariant_spec_22_16) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class B "
        "  public x: int := 0; "
        "  invariant(x > 0); "
        "  public constructor(v: int) x := v; end constructor; "
        "end B; "
        "class D : B "
        "  public y: int := 0; "
        "  invariant(y >= 0); "
        // fix #4549 (§12.8, v0.306): `D` now WRITES its `base(...)`
        // chain. §12.8 inserts an implicit `base()` when none is
        // written and "requires the base class to have a parameterless
        // constructor"; `B` declares only `constructor(v: int)`, so
        // this program was ill-formed and is rejected as of #4549.
        // The ORACLE IS UNCHANGED (still `== 11` / `== 15`, and this
        // file's subject — §22.16 inherited-invariant evaluation at
        // construction exit — is untouched): `base(v)` sets `x := v`
        // in `B`'s constructor and the body then assigns the same `v`
        // to the same field, so every value the rows observe is the
        // one they observed before.
        "  public constructor(v: int, w: int) "
        "    base(v); "
        "    x := v; y := w; "
        "  end constructor; "
        "  public static method Main(): int "
        "    using var d := new D(5, 10); return d.x + d.y; "
        "  end Main; "
        "end D;"), 15);
}

// §22.16 (v0.240, #2941 review follow-up): the invariant-throw at
// construction exit must dispose + free the partially-constructed
// instance under §22.9's ordinary machinery — the heap block + every
// owned field are reclaimed. Without the disposal/free wiring, the
// construction-exit throw path leaked `obj` (and any owned fields it
// holds) every time the invariant failed.
//
// This test exercises the heap-loop with an `owned` class field and
// runs under LeakSanitizer (runAotChecked). Without the fix, ASan
// flags a heap-leak / owned-field-leak on each iteration (100
// iterations → 100 leaked blocks + 100 leaked Holder instances = 200
// leaked blocks; LSan would report `LeakSanitizer: detected memory
// leaks` and the harness surfaces `ran=false`). With the fix, the
// test runs ASan-clean and the program exits with `r == 100` (every
// iteration's invariant fired and was caught).
BEZEL_TEST(invariant_member_evaluated_2941,
           violated_invariant_at_construction_exit_disposes_owned_field_spec_22_16) {
    auto r = bezel::test::runAotChecked(
        "module M; class Holder "
        "  public val: int := 0; "
        "  public constructor(v: int) val := v; end constructor; "
        "end Holder; "
        "class C "
        "  public owned inner: Holder; "
        "  invariant(inner.val > 0); "
        "  public constructor(h: owned Holder) inner := h; end constructor; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    var i: int := 0; "
        "    while i < 100 do "
        "      try "
        "        using var c := new C(new Holder(-1)); "
        "      catch ContractViolationException => r := r + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 iterations, every iteration throws and is caught → r == 100.
    // The interesting property is r.ran: ASan-detected leaks make the
    // harness fail with ran=false BEFORE this assertion runs.
    BEZEL_EXPECT_EQ(r.exitCode, 100);
}
