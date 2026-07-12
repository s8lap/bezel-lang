// Codegen regression for #1766 — a generic base class's VIRTUAL method returning
// the type parameter `T`, OVERRIDDEN in a derived class where `T` is a REFERENCE
// type (spec §12 Inheritance and Dispatch + §14 Generics, v0.188).
//
// Bug: declareVtables installed a value-widening adapter thunk (its sole purpose
// is unboxing widened nullable-VALUE params, #1592) for the override's slot even
// when NO parameter was widened, because the base's un-instantiated `GetVal(): T`
// lowers its slot return type to the erased `i32` placeholder while the
// reference-typed override (`T` = string) returns `ptr`. The pass-through thunk
// then emitted `ret ptr %vw.ret` from an `i32`-typed function — malformed IR.
// The AOT verifier (Driver::compileFile → emitObjectFile) rejects it; ORC's
// LLJIT SKIPS the verifier, so the JIT silently ran the malformed IR (the
// ptr/i32 return happened to alias in a register). A genuine miscompile the JIT
// masked, surfaced by the JIT→AOT shadow-compare (migration P0-E).
//
// Harness: runAot is REQUIRED here — it drives the real bezelc driver, which
// runs llvm::verifyModule, so a fix regression re-fails the verifier. Under
// runJit (no verifier) these programs pass even WITHOUT the fix (that is exactly
// why the defect stayed latent). Fix: ensureVtableWideningThunk bails when no
// position is actually widened (LlvmBackend_Setup.cpp), leaving the override fn
// installed directly; the opaque-`ptr` vtable slot needs no adapter.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §12/§14: `Base<T>.GetVal(): T` overridden by `Derived: Base<string>` with
// `GetVal(): string`. Before the fix the module failed IR verification under
// AOT (run() → -999); with the fix the override round-trips the string and
// "hi".Length == 2. The override need only be EMITTED to trigger the bad IR, but
// this program also CALLS it so the returned value is checked end to end.
BEZEL_TEST(codegen_generic_virtual_override_1766,
           reftype_generic_T_virtual_override_returns_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base<T> "
        "  public V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "  public virtual method GetVal(): T return V; end GetVal; "
        "end Base; "
        "class Derived: Base<string> "
        "  public constructor(v: string) base(v); end constructor; "
        "  public override method GetVal(): string return V; end GetVal; "
        "end Derived; "
        "class C public static method Main(): int "
        "  var d := new Derived(\"hi\"); var s := d.GetVal(); var r := s.Length; "
        "  d.Dispose(); return r; "
        "end Main; end C;"), 2);
}

// §12.3: the same shape overriding via `return base.GetVal();` (the original
// corpus program) — the virtual base call must also lower `T`→ptr for a
// `Base<string>` receiver. "hi".Length == 2.
BEZEL_TEST(codegen_generic_virtual_override_1766,
           reftype_generic_T_virtual_override_via_base_call) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base<T> "
        "  public V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "  public virtual method GetVal(): T return V; end GetVal; "
        "end Base; "
        "class Derived: Base<string> "
        "  public constructor(v: string) base(v); end constructor; "
        "  public override method GetVal(): string return base.GetVal(); "
        "  end GetVal; "
        "end Derived; "
        "class C public static method Main(): int "
        "  var d := new Derived(\"hi\"); var s := d.GetVal(); var r := s.Length; "
        "  d.Dispose(); return r; "
        "end Main; end C;"), 2);
}

// CONTROL (must NOT regress): the identical shape with a VALUE type argument
// (`Base<int>`, `override GetVal(): int`) — `int` lowers to `i32`, the base slot
// and override agree, no thunk, no mismatch. Compiled cleanly before the fix and
// must keep doing so. GetVal → 10.
BEZEL_TEST(codegen_generic_virtual_override_1766,
           valuetype_generic_T_virtual_override_unregressed) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base<T> "
        "  public V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "  public virtual method GetVal(): T return V; end GetVal; "
        "end Base; "
        "class Derived: Base<int> "
        "  public constructor(v: int) base(v); end constructor; "
        "  public override method GetVal(): int return V; end GetVal; "
        "end Derived; "
        "class C public static method Main(): int "
        "  var d := new Derived(10); var r := d.GetVal(); d.Dispose(); return r; "
        "end Main; end C;"), 10);
}
