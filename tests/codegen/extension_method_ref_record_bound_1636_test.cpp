// fix #1636 / §27.6 / §26.1: a BOUND extension-method reference on a RECORD
// instance — `v.Scaled` used as a `() -> int` value, where `v` is a record
// local and `Scaled` is an extension method on the record's type — must lower
// to a valid function value that, when called, invokes `Scaled` on a COPY of
// `v`. §27.6: "instance methods (the instance is captured at formation — by
// reference for class-typed receivers, by copy for value-shaped (record)
// receivers...)". §26.1 shapes an extension method's `this` for a record
// receiver as a POINTER TO A SPILLED COPY of the value (so the body's
// `this.field` reuses ordinary record field access).
//
// BUG (#1636): distinct from #1306 (the UNBOUND type-qualified form,
// `Vec.Scaled`, where the receiver is the function's first PARAMETER) and from
// #457 (the BOUND PRIMITIVE form, `x.Double`, where the captured receiver is a
// raw scalar). For the BOUND RECORD form, `makeMethodThunk`'s instance-method
// path always loaded a `ptr` from the closure env — correct for a class
// receiver (env holds the object pointer directly) and for a primitive/enum
// receiver (env holds the raw value, loaded with its own type per #457's fix)
// — but WRONG for a record receiver: `buildMethodClosureCell` stores the
// record's STRUCT VALUE itself into env (matching #27.6's "captured... by
// copy"), not a pointer. Loading `ptr` there reinterprets the struct's leading
// bytes as a garbage pointer, and the extension method's body (`this.field`,
// a GEP off that pointer) dereferences it — SIGSEGV at call time.
//
// Fix: when the extension's target type is a record, load the record's actual
// struct type from env (not `ptr`), then spill that loaded value into a fresh
// stack slot and pass its address as `this` — mirroring the record-receiver
// shaping already applied at direct (non-thunk) extension-method call sites
// and at the #1306 unbound-thunk path.
//
// These tests JIT-run programs that bind an extension-method reference on a
// record local and call it; without the fix they crash (ran == false, the JIT
// harness catches the SIGSEGV and reports it as a failed run).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Run a whole module source and return Main's exit code, or a sentinel if the
// program crashed / failed to run.
int runMod(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §27.6 / §26.1 (#1636): the core repro from the issue — `v.Scaled` bound to
// `() -> int` and called. Before the fix: SIGSEGV (ran == false, -999).
// After the fix: Scaled(v) = v.X*2 + v.Y*2 = 1*2 + 2*2 = 6.
BEZEL_TEST(codegen_extension_method_ref_record_bound_1636,
           bound_record_ext_ref_called_1636) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "record Vec(X: int, Y: int) end; "
        "extension Vec "
        "  public method Scaled(): int => this.X * 2 + this.Y * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var v: Vec := new Vec(1, 2); "
        "    var f: () -> int := v.Scaled; "
        "    return f(); "
        "  end Main; "
        "end C;"), 6);
}

// §27.6 (#1636): a bound record extension-method reference passed directly as
// a call argument (not first stored to a local) — exercises the same thunk
// path through a different syntactic route.
BEZEL_TEST(codegen_extension_method_ref_record_bound_1636,
           bound_record_ext_ref_passed_as_arg_1636) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "record Vec(X: int, Y: int) end; "
        "extension Vec "
        "  public method Scaled(): int => this.X * 2 + this.Y * 2; "
        "end; "
        "class C "
        "  public static method Apply(f: () -> int): int return f(); end Apply; "
        "  public static method Main(): int "
        "    var v: Vec := new Vec(3, 4); "
        "    return Apply(v.Scaled); "  // 3*2 + 4*2 = 14
        "  end Main; "
        "end C;"), 14);
}

// §27.6 (#1636): the reference captures the record BY COPY at formation — a
// mutation to the original local's field after formation must NOT be observed
// by the already-formed function value (distinguishing record "by copy"
// capture from a class receiver's "by reference" capture, per §27.6).
BEZEL_TEST(codegen_extension_method_ref_record_bound_1636,
           bound_record_ext_ref_captures_by_copy_1636) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "record Vec(X: int, Y: int) end; "
        "extension Vec "
        "  public method Scaled(): int => this.X * 2 + this.Y * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var v: Vec := new Vec(1, 1); "
        "    var f: () -> int := v.Scaled; "  // captures a COPY: X=1,Y=1 -> 4
        "    v := new Vec(100, 100); "        // reassigning v must not affect f
        "    return f(); "                    // still 4, not 400
        "  end Main; "
        "end C;"), 4);
}

// Regression (§27.6/#457): a bound PRIMITIVE extension-method reference must
// remain unaffected by the record-shaping branch added for #1636.
BEZEL_TEST(codegen_extension_method_ref_record_bound_1636,
           regression_primitive_bound_ext_ref_1636) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "extension int "
        "  public method Double(): int => this * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var x := 21; "
        "    var f: () -> int := x.Double; "
        "    return f(); "  // 42
        "  end Main; "
        "end C;"), 42);
}

// Regression (§27.6/§26.1): a bound CLASS extension-method reference must
// remain unaffected — env still stores the object pointer directly and the
// default `ptr` load path (no spill) still applies.
BEZEL_TEST(codegen_extension_method_ref_record_bound_1636,
           regression_class_bound_ext_ref_1636) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "class Box "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "extension Box "
        "  public method Tripled(): int => this.N * 3; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(14); "
        "    var f: () -> int := b.Tripled; "
        "    return f(); "  // 42
        "  end Main; "
        "end C;"), 42);
}

// Regression (§27.6/#1306): the UNBOUND type-qualified extension-method
// reference (`Vec.Scaled`) must remain unaffected — its receiver travels as
// the thunk's first user parameter (extRef path), never through the
// bound-instance env-load path this fix touches.
BEZEL_TEST(codegen_extension_method_ref_record_bound_1636,
           regression_unbound_record_ext_ref_1636) {
    BEZEL_EXPECT_EQ(runMod(
        "module M; "
        "record Vec(X: int, Y: int) end; "
        "extension Vec "
        "  public method Scaled(): int => this.X * 2 + this.Y * 2; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    var g: (Vec) -> int := Vec.Scaled; "
        "    return g(new Vec(5, 5)); "  // 5*2 + 5*2 = 20
        "  end Main; "
        "end C;"), 20);
}
