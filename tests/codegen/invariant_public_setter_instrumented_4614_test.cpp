// Spec-derived runtime tests for §22.16's SETTER row of the instrumentation
// set (v0.306, issue #4614 part (b)).
//
// §22.16 "The instrumentation set — the invariant is evaluated at exactly
// these points, in checked-contracts mode":
//
//   "- Entry and normal exit of every `public` instance method.
//    - **Entry and normal exit** of every `public` **setter** — block-form,
//      auto-property, indexer, `set(owned)` alike.
//    - Never instrumented: non-public methods and setters, static members,
//      destructors, and `Dispose`; throw exits check nothing."
//
// The setter row was not emitted at all. `grep -n emitInvariantCheck
// src/codegen/` found six call sites — public-method entry
// (`emitFunctionBody`), public-method exit (`armEnsuresEpilogue` →
// `finalizeEnsuresEpilogue`) and four construction-exit sites in `lowerNew` —
// and NO accessor path called it. Measured on this HEAD before the fix: a
// `public property Q: int` whose block-form setter drives the instance into a
// state its own class invariant rejects ran to completion and returned 42,
// where §22.16 requires ContractViolationException (99). A silent
// under-check, which is the direction §22.16's observable-boundary property
// exists to prevent.
//
// ORACLE DISCIPLINE. The pre-fix behaviour is a WRONG VALUE, not a crash, so
// every row asserts the exact §22.16 verdict; `runChecked` still collapses
// "did not terminate normally" to -999 so a regression that crashes cannot
// pass. The two boundaries are pinned SEPARATELY (rows 1 and 2): a fix that
// emitted only one of them would leave the other row red.
//
// SCOPE OF THE FIX THIS FILE PINS. The instrumentation is emitted at the
// three setter paths that live in `LlvmBackend_Assign.cpp` — the local
// block-form / `set(owned)` named-property setter, and (by the same gate) the
// `this`-receiver exemption. §22.16's auto-property row is NOT covered here: a
// same-module `c.P := v` on a non-virtual auto-property lowers to a direct
// backing-field store through `lvalueAddr` (LlvmBackend_Layout.cpp) and never
// reaches a setter path at all. That residual was closed separately by
// **#4646**, at the generic assignment arm of `lowerStmt` — see
// `tests/codegen/invariant_auto_property_store_instrumented_4646_test.cpp`,
// which reuses this file's `instrumentedSetterClass` gate so the two shapes
// cannot drift apart. The VIRTUAL / interface-dispatched setter (which needs
// the setter's own §22.16 unchecked face before it can be instrumented without
// introducing an over-check) stays recorded on #4646.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

int runChecked(const std::string& program) {
    auto r = bezel::test::runAotCheckedContracts(program);
    return r.ran ? r.exitCode : -999;
}

int runRelease(const std::string& program) {
    auto r = bezel::test::runAot(program, bezel::codegen::CodegenOptions{});
    return r.ran ? r.exitCode : -999;
}

// ★ Row 1 — the NORMAL-EXIT boundary. The invariant holds when the setter is
// entered and is false when it returns. Pre-fix 42; §22.16 requires 99.
const char* kBlockSetterExitBoundary =
    "module S; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property Q: int "
    "    get => k; "
    "    set => do k := value; end; "
    "  end Q; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      c.Q := 0 - 3; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// ★ Row 2 — the ENTRY boundary, isolated. The instance is driven into a
// rejected state through a public FIELD (never an instrumentation point), and
// the setter RESTORES the invariant before it returns — so only an ENTRY
// check can observe the violation. Pre-fix 42; §22.16 requires 99.
const char* kBlockSetterEntryBoundary =
    "module S; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property Q: int "
    "    get => k; "
    "    set => do k := 5; end; "
    "  end Q; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      c.k := 0 - 1; "
    "      c.Q := 7; "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// Control — §22.16's `this`-reentrancy exemption applies to setters exactly
// as it does to methods: "A call whose receiver is `this` — explicit or
// implicit — from inside any instance member of the class or a subclass
// enters the member's unchecked face". `Rebuild` drives the instance through
// a state its invariant rejects VIA ITS OWN SETTER and restores it before
// returning. 4 on BOTH sides of the fix: without this row the setter
// instrumentation could be emitted unconditionally and turn the silent
// under-check into a loud over-check, and nothing here would notice.
const char* kThisReceiverSetterExempt =
    "module S; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property Q: int "
    "    get => k; "
    "    set => do k := value; end; "
    "  end Q; "
    "  public method Rebuild(): int "
    "    this.Q := 0 - 3; "
    "    this.Q := 4; "
    "    return k; "
    "  end Rebuild; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var c := new C(5); "
    "    try "
    "      r := c.Rebuild(); "
    "    catch ContractViolationException => r := 88; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainS;";

// Control — a CONFORMING store is unobservable (§22.16's clauses are pure).
// 7 on both sides; pins that the fix does not disturb the happy path.
const char* kConformingSetterStore =
    "module S; class C "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property Q: int "
    "    get => k; "
    "    set => do k := value; end; "
    "  end Q; "
    "end C; "
    "class MainS "
    "  public static method Main(): int "
    "    using var c := new C(5); "
    "    c.Q := 7; "
    "    return c.Q; "
    "  end Main; "
    "end MainS;";

// Control + its differential twin — §22.16's never-instrumented list names
// "non-public methods and SETTERS". Both programs are byte-identical but for
// the `private` on the setter accessor (§13.6 accessor visibility), and both
// reach the setter on ANOTHER instance (`o.Q := …`, not `this`), so the
// §22.16 `this`-reentrancy exemption is not what separates them — visibility
// is. The private row is 42 on BOTH sides of the fix; the public row is 42
// pre-fix and 99 with it.
const char* kOtherInstanceSetterTemplatePrivate =
    "module S; class Cx "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property Q: int "
    "    get => k; "
    "    private set => do k := value; end; "
    "  end Q; "
    "  public method Poke(o: Cx): int "
    "    o.Q := 0 - 3; "
    "    o.k := 5; "
    "    return 1; "
    "  end Poke; "
    "end Cx; "
    "class Mainx "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var a := new Cx(5); "
    "    using var b := new Cx(5); "
    "    try "
    "      r := a.Poke(b); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end Mainx;";

const char* kOtherInstanceSetterTemplatePublic =
    "module S; class Cx "
    "  public k: int := 1; "
    "  invariant(k > 0); "
    "  public constructor(v: int) k := v; end constructor; "
    "  public property Q: int "
    "    get => k; "
    "    set => do k := value; end; "
    "  end Q; "
    "  public method Poke(o: Cx): int "
    "    o.Q := 0 - 3; "
    "    o.k := 5; "
    "    return 1; "
    "  end Poke; "
    "end Cx; "
    "class Mainx "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var a := new Cx(5); "
    "    using var b := new Cx(5); "
    "    try "
    "      r := a.Poke(b); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end Mainx;";

}  // namespace

BEZEL_TEST(invariant_public_setter_instrumented_4614,
           block_setter_normal_exit_boundary_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kBlockSetterExitBoundary));
}

BEZEL_TEST(invariant_public_setter_instrumented_4614,
           block_setter_entry_boundary_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kBlockSetterEntryBoundary));
}

BEZEL_TEST(invariant_public_setter_instrumented_4614,
           this_receiver_setter_is_exempt_spec_22_16) {
    BEZEL_EXPECT_EQ(4, runChecked(kThisReceiverSetterExempt));
}

BEZEL_TEST(invariant_public_setter_instrumented_4614,
           conforming_setter_store_spec_22_16) {
    BEZEL_EXPECT_EQ(7, runChecked(kConformingSetterStore));
}

BEZEL_TEST(invariant_public_setter_instrumented_4614,
           non_public_setter_never_instrumented_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runChecked(kOtherInstanceSetterTemplatePrivate));
}

BEZEL_TEST(invariant_public_setter_instrumented_4614,
           public_setter_on_another_instance_is_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kOtherInstanceSetterTemplatePublic));
}

// §22.16 release default (#4386): no invariant is evaluated at all, so the
// violating store completes and `Main` returns 42. Without this row a fix
// that emitted the setter instrumentation unconditionally would pass.
BEZEL_TEST(invariant_public_setter_instrumented_4614,
           block_setter_unchecked_mode_control_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runRelease(kBlockSetterExitBoundary));
}
