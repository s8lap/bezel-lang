// Regression tests for #4056 (§9.4 / §22.3 / CG29 / v0.294) — passing `this`
// as an argument from inside a record's own constructor crashes codegen with
// "Call parameter type does not match function signature! / IR verification
// failed".
//
// §9.4: "A record is a value type. Records have value semantics: assignment
// copies the value." §22.3 / CG29: inside a record method or constructor the
// receiver `this` is lowered to a POINTER to the by-value receiver slot (spilled
// so `this.field` reuses ordinary record field access). The same trick is the
// post-#1759 return-value path: `return this`, `field := this`, `var q := this`
// all flow through `coerceThisRecordValue` (`LlvmBackend_Layout.cpp` ~L2301)
// which loads the struct out of `*this` exactly when the destination is the
// receiver record's struct layout.
//
// Before this fix the CALL ARGUMENT path did not load: `this` lowered to
// `currentThis_` (a `ptr`) flowed through `buildOrderedArgs` unchanged, then
// through `coerceExprValueToSlot`'s numeric-only narrowing/widening block
// without touching it. The callee's declared parameter type is the record
// STRUCT (records are passed by value, §9.4); the actual arg was a `ptr`; LLVM
// rejected the module. Reproducible on every shape:
//
//   * static-method argument: `R.Touch(this)` — Touch declared
//     `Touch(x: R): int`, call emitted `Touch(ptr %this)` against
//     `Touch(%record.R)`.
//   * instance-method argument: `this.Combine(this)` — Combine declared
//     `Combine(other: R): int`, call emitted `Combine(ptr %this, ptr %this)`
//     against `Combine(ptr, %record.R)` (the SECOND arg is the bug; the first
//     is the receiver and is supposed to be `ptr`).
//   * a stored local copy of `this` passed as an argument works fine: it is
//     already the struct, so `coerceExprValueToSlot` is a no-op.
//
// The repro is independent of definite-assignment order: #4014's sema fix
// closes the unassigned-field path before codegen ever sees this, but the
// fields-already-assigned admitted case still crashed. Pinning that case
// here is exactly what #4014's own `tests/sema/record_ctor_this_observation_gate_4014_test.cpp`
// noted in passing as "reaches a SEPARATE, pre-existing codegen defect
// (malformed IR, filed as its own issue)". This file is that issue.
//
// AOT is the correct oracle — the bug is a front-end IR-verifier abort that
// the shipping bezelc path enforces. All observables fit an 8-bit exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Shared helper class — a reference-typed field with a single int, mirroring
// the structure kWidget4014 in the sema-level #4014 regression test (so a
// reader who reads the two together sees the same shape in both).
const char* kWidget4056 =
    "module M; "
    "public class Widget4056 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4056; ";
}  // namespace

// Static-method variant — the issue's headline repro. `this` passed as a
// record-typed argument to a STATIC method of the same record. The callee
// expects the record by value; before the fix the call emitted `Touch(ptr
// %this)` against `Touch(%record.R)` and LLVM's verifier aborted the module.
// After the fix `currentThis_` is loaded into the record struct at the call
// site (the same way it is for `return this`, fix #1759), and the program
// runs to completion returning the round-tripped Count (42).
BEZEL_TEST(record_this_pass_as_arg_4056, static_method_arg_runs) {
    BEZEL_EXPECT_EQ(runFull(std::string(kWidget4056) +
        "public record R4056 "
        "  public readonly property Extra: Widget4056; "
        "  public static method Touch(x: R4056): int return x.Extra.Render(); end Touch; "
        "  public constructor(w: Widget4056) "
        "    Extra := w; "
        "    var y := R4056.Touch(this); "
        "  end constructor; "
        "end R4056; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4056(42); "
        "    var c := new R4056(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;"), 42);
}

// Instance-method variant — `this` passed as a non-receiver argument to an
// instance method of the same record (implicit-`this` call, `Combine(this)`).
// The instance method's declared signature is `(ptr, %record.R)`; before the
// fix the second arg was a `ptr` (the receiver pointer) where the callee
// expected the record struct, and LLVM's verifier aborted the module.
// Mirrors the static-method case but rules out "static-only": instance
// dispatch uses the same coercion path with `args[1]` (a non-receiver
// record-typed parameter) as the broken arg.
BEZEL_TEST(record_this_pass_as_arg_4056, instance_method_arg_runs) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public record R4056i "
        "  public readonly property Extra: int; "
        "  public constructor(w: int) "
        "    Extra := w; "
        "    var y := Combine(this); "
        "  end constructor; "
        "  public method Combine(other: R4056i): int return other.Extra; end Combine; "
        "end R4056i; "
        "class C "
        "  public static method Main(): int "
        "    var c := new R4056i(42); "
        "    return c.Extra; "
        "  end Main; "
        "end C;"), 42);
}

// Class control — the same shape on a CLASS receiver passes `this` as a
// pointer everywhere (CG29's record-only quirk), so no struct load is
// required and the program worked both pre- and post-fix. Guards the fix
// against regressing the class path (where currentThis_ IS the right
// argument type).
BEZEL_TEST(record_this_pass_as_arg_4056, class_receiver_unchanged) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public class Box4056 "
        "  public Value: int; "
        "  public constructor(v: int) Value := v; end constructor; "
        "  public method Render(): int return Value; end Render; "
        "end Box4056; "
        "class C "
        "  public static method Touch(x: Box4056): int return x.Render(); end Touch; "
        "  public static method Main(): int "
        "    var b := new Box4056(42); "
        "    return Touch(b); "
        "  end Main; "
        "end C;"), 42);
}
