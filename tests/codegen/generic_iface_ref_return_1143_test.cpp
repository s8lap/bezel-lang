// Codegen — generic interface method returning a class-typed type parameter T
// dispatched through the interface variable produces a correct reference
// (issue #1143).
//
// Root cause: the shared itable for Box<T>$IGetter<T> stored the TEMPLATE
// function bz$Box$Get compiled with T→i32 (i32 placeholder). When Box<Elem>
// is constructed, specializedVtableFor only specialised the vtable when the
// class had an OWNED type-param field; a plain (non-owned) reference-typed
// field was not detected, so the object header still pointed at the shared
// `vt$Box` whose slot 1 (itmap) contained the template itable. Dispatching
// `g.Get()` through the interface called the i32-erased function, which:
//   (1) read the field at the WRONG struct offset (i32 slot vs ptr slot), and
//   (2) returned a truncated 32-bit integer instead of the full 64-bit pointer.
// The caller typed the return as `ptr` (methodFnType used the active concrete
// genericSubst_), so the truncated i32 was widened into garbage — SIGSEGV on
// any vtable access through the returned value.
//
// Fix (specializedVtableFor): a second trigger condition (b) was added.  When
// any interface method's concrete function type (under genericSubst_) differs
// from the template function's type, per-instantiation itables are built
// (replacing only the slots that differ) and a new itmap/vtable clone is
// installed into the object.  Condition (a) (owned T field → specialized
// disposer) is unchanged.
//
// Spec references:
//   §12 — interface itable dispatch
//   §13 — generic interface conformance
//   §14 — generic type/method monomorphization: every T reference resolves to
//          the concrete type at the call site; function types must agree end-to-
//          end.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── (a) basic: Get() returns a class reference through IGetter<T> ────────────
// §14/§12: IGetter<Elem>.Get() dispatched through an interface variable must
// return the actual Elem pointer; calling a virtual method on it must work.
// Before the fix: SIGSEGV (rc=139) — the returned pointer was garbage.
BEZEL_TEST(generic_iface_ref_return_1143, basic_iface_dispatch_returns_class_ref) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IGetter<T> method Get(): T; end IGetter; "
        "class Box<T>: IGetter<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Get(): T => _v; "
        "end Box; "
        "class Elem "
        "  public virtual method M(): int => 7; "
        "end Elem; "
        "class App "
        "  public static method Main(): int "
        "    using var t := new Elem(); "
        "    using var b := new Box<Elem>(t); "
        "    var g: IGetter<Elem> := b; "
        "    var x := g.Get(); "
        "    return x.M(); "
        "  end Main; "
        "end App;"), 7);
}

// ── (b) value type param: Box<int> via IGetter<int> still works ──────────────
// §14: when T→int the template function type matches the concrete type (i32);
// no itable specialization is needed. The result must still be correct.
BEZEL_TEST(generic_iface_ref_return_1143, value_type_param_int_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IGetter<T> method Get(): T; end IGetter; "
        "class Box<T>: IGetter<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Get(): T => _v; "
        "end Box; "
        "class App "
        "  public static method Main(): int "
        "    using var b := new Box<int>(42); "
        "    var g: IGetter<int> := b; "
        "    return g.Get(); "
        "  end Main; "
        "end App;"), 42);
}

// ── (c) two consecutive itable-dispatch calls both return valid refs ──────────
// §12/§14: repeated calls through the same interface variable must each return
// the correct reference (the specialised itable is cached; no double-build).
BEZEL_TEST(generic_iface_ref_return_1143, repeated_iface_call_both_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IGetter<T> method Get(): T; end IGetter; "
        "class Box<T>: IGetter<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Get(): T => _v; "
        "end Box; "
        "class Elem "
        "  public _n: int := 0; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public virtual method M(): int => _n; "
        "end Elem; "
        "class App "
        "  public static method Main(): int "
        "    using var t := new Elem(3); "
        "    using var b := new Box<Elem>(t); "
        "    var g: IGetter<Elem> := b; "
        "    var x := g.Get(); "
        "    var y := g.Get(); "
        "    return x.M() + y.M(); "
        "  end Main; "
        "end App;"), 6);
}
