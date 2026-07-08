// Codegen — the #1499 fix's DISPOSAL correctness, via runtime destructor
// counts (JIT exit code). §28.3 "Factory methods and sourceless returns" /
// Appendix C.3: an unannotated STATIC method whose return is a FRESH owned
// value is treated as owned by the caller (the same discharge rules as `new`),
// so a `using var` bound to it disposes exactly once. A static method that
// returns a BORROW (a static-field read, a receiver-sourced value) is NOT
// owned — disposing it would double-free a value the callee does not own
// (§28.10). This file pins BOTH halves at runtime.
//
// The classification lives once in common::staticFactoryReturnsFreshOwned and
// is consulted by the sema ownership pass AND the codegen disposal-registration
// mirrors (usingVarOwnsClassObjectDynamically / unwindOwnedLocalClass) so the
// sema classification and the actual disposal never diverge. `using var` needs
// no ownership pass to register its scope-end disposal (`ld->isUsing` drives
// it), so a plain runJit already exercises the fixed codegen path end-to-end.

#include <string>

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A plain disposable class whose destructor bumps a static counter, so the JIT
// exit code observes the exact number of disposals with no I/O.
const char* kRes =
    "module M; class Res "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res._n := Res._n + 1; end destructor; "
    "end Res; ";

}  // namespace

// --- Owned-factory half (Appendix C.3 / §28.3): FRESH return is disposed. ---

// The issue's repro shape: an unannotated static factory returns `new Res()`;
// its result, bound via `using var`, is disposed exactly once at scope end.
BEZEL_TEST(codegen_sourceless_static_factory_1499, using_var_fresh_disposes_once) {
    std::string src = std::string(kRes) +
        "class C public static method Make(): Res return new Res(); end Make; "
        "end C; "
        "class D "
        "  static method f() using var m := C.Make(); end f; "
        "  public static method Main(): int f(); return Res._n; end Main; "
        "end D;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

// Two independent factory calls, each `using var`-bound in its own scope, each
// dispose exactly once — no double-free, no missed disposal.
BEZEL_TEST(codegen_sourceless_static_factory_1499, two_fresh_calls_dispose_twice) {
    std::string src = std::string(kRes) +
        "class C public static method Make(): Res return new Res(); end Make; "
        "end C; "
        "class D "
        "  static method f() using var m := C.Make(); end f; "
        "  public static method Main(): int f(); f(); return Res._n; end Main; "
        "end D;";
    BEZEL_EXPECT_EQ(run(src), 2);
}

// An expression-bodied static factory (`=> new Res()`) is the same fresh-owned
// shape and is likewise disposed once.
BEZEL_TEST(codegen_sourceless_static_factory_1499, expr_body_fresh_disposes_once) {
    std::string src = std::string(kRes) +
        "class C public static method Make(): Res => new Res(); end C; "
        "class D "
        "  static method f() using var m := C.Make(); end f; "
        "  public static method Main(): int f(); return Res._n; end Main; "
        "end D;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

// A static factory declared with an INTERFACE return type, returning a fresh
// `new` of an implementing class, is owned too — disposal dispatches through
// the object's own vtable (the interface-return sentinel path).
BEZEL_TEST(codegen_sourceless_static_factory_1499, interface_return_fresh_disposes_once) {
    std::string src =
        "module M; interface IShape end IShape; "
        "class Sq : IShape "
        "  public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Sq._n := Sq._n + 1; end destructor; "
        "end Sq; "
        "class C public static method Make(): IShape return new Sq(); end Make; "
        "end C; "
        "class D "
        "  static method f() using var m := C.Make(); end f; "
        "  public static method Main(): int f(); return Sq._n; end Main; "
        "end D;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

// --- Borrow-return half (§28.10): the REGRESSION GUARD. ---

// A static method whose body returns a static-field READ (`_shared`) is a
// BORROW, not a fresh owned value. `using var r := Get();` must NOT dispose it
// — the field's process-lived object is owned by the static storage, and
// disposing the borrow would double-free it (this is exactly the soundness
// regression the previous #1499 attempt introduced by classifying EVERY
// unannotated static return owned). The destructor must not run inside Main:
// Res._n stays 0 at return. (The static `_shared` is reclaimed only at
// program-exit static disposal, after Main's value is captured.)
BEZEL_TEST(codegen_sourceless_static_factory_1499, borrow_field_return_not_disposed) {
    std::string src = std::string(kRes) +
        "class Holder "
        "  public static readonly _shared: Res := new Res(); "
        "  public static method Get(): Res => Holder._shared; "
        "  public static method Main(): int "
        "    do using var r := Get(); end; return Res._n; "
        "  end Main; "
        "end Holder;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// Same borrow-return through a receiver-sourced INSTANCE method: `h.Get()`
// borrows the receiver's owned field, so the `using var` does not dispose it;
// the field is disposed once when the owner `h` is disposed (Res._n == 1, not
// 2 — no double-free from mis-classifying the borrow as owned).
BEZEL_TEST(codegen_sourceless_static_factory_1499, instance_borrow_return_disposed_once_by_owner) {
    std::string src = std::string(kRes) +
        "class Holder "
        "  public owned _r: Res; "
        "  public constructor() this._r := new Res(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): Res => _r; "
        "end Holder; "
        "class D public static method Main(): int "
        "  do using var h := new Holder(); using var b := h.Get(); end; "
        "  return Res._n; "
        "end Main; end D;";
    BEZEL_EXPECT_EQ(run(src), 1);
}
