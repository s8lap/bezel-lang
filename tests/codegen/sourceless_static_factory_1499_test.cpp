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

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
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

// --- Borrow-return half: the REGRESSION GUARD. ---

// (Removed in v0.196, #1815.) The former `borrow_field_return_not_disposed`
// case compiled a static method returning a static-field read
// (`static method Get(): Res => Holder._shared;`) and observed that the
// `using var`-bound borrow was not disposed. v0.196 retires the static-method
// static-storage re-exposure arm (§28.3 ~L5898 / §28.6 ~L6085), so that program
// no longer compiles — it is rejected by the ownership pass before codegen. A
// rejected program never reaches codegen, so the codegen-side guard is gone;
// the reject is pinned instead in tests/ownership/sourceless_static_factory_
// 1499_test.cpp (`static_owned_field_reexposed_by_method_rejected` and the
// flipped `borrow_field_return_stays_borrowed`). The receiver-sourced INSTANCE
// borrow below is unaffected (instance methods are outside this rule) and still
// exercises the not-disposed-until-owner-disposes disposal path.

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
