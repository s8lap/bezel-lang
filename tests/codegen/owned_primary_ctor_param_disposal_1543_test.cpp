// Codegen tests for bug #1543 — an `owned` primary-constructor parameter that
// is referenced in a *body* position is captured into a compiler-generated
// `owned` private field and disposed by the generated `Dispose()` (§28.4), in
// reverse declaration order after the class's declared fields. Authority:
// §22.10 (v0.180), §11.1, §28.4/§28.5.
//
// The repro from bug #1543:
//   class Holder(owned r: Res) with `Use(): int => r.Id();`
// used to compile with zero diagnostics while `Res`'s destructor never ran —
// the captured parameter had undefined ownership and leaked. §22.10 (v0.180)
// makes the capture an `owned` field disposed with the object.
//
// Each Res destructor bumps a static counter; `Main` returns the counter so
// disposal is observable as the exit code. A double-dispose would be a heap
// error (or wrong count), so the counts also verify disposal happens *exactly
// once* — the ResourceHolder / borrow / idempotency cases below are the
// no-double-free guards.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A disposable class whose destructor bumps a static counter, shared by the
// programs below. Kept as a string fragment so each test composes its own Main.
const char* kRes =
    "class Res "
    "  public static _n: int := 0; "
    "  private _id: int; "
    "  public constructor(id: int) _id := id; end constructor; "
    "  public method Id(): int => _id; "
    "  destructor _n := _n + 1; end destructor; "
    "end Res; ";

} // namespace

// §22.10 (v0.180) — the exact repro from bug #1543: an `owned` primary-ctor
// parameter referenced in a method body is captured as an owned field and
// disposed with the object exactly once (counter → 1).
BEZEL_TEST(codegen_owned_primary_ctor_param_1543, body_ref_owned_param_disposed_22_10) {
    // per §22.10 (v0.180) / §28.4
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kRes +
        "class Holder(owned r: Res) "
        "  public method Use(): int => r.Id(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(new Res(7)); end; "
        "  return Res._n; "
        "end Main; end C;"), 1);
}

// §22.10 (v0.180) — two `owned` primary-ctor parameters captured in a body:
// both are disposed (counter → 2).
BEZEL_TEST(codegen_owned_primary_ctor_param_1543, two_owned_params_captured_22_10) {
    // per §22.10 (v0.180) / §28.4
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kRes +
        "class Holder(owned a: Res, owned b: Res) "
        "  public method Sum(): int => a.Id() + b.Id(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(new Res(3), new Res(4)); end; "
        "  return Res._n; "
        "end Main; end C;"), 2);
}

// §22.10 (v0.180) — a plain (non-`owned`) primary-ctor parameter captured in a
// body is a borrow: it is NOT disposed by the capturing class. Only the `owned`
// parameter is disposed (counter → 1), and the plain `int` tag has no effect.
BEZEL_TEST(codegen_owned_primary_ctor_param_1543, plain_param_unaffected_22_10) {
    // per §22.10 (v0.180) / §28.5
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kRes +
        "class Holder(tag: int, owned r: Res) "
        "  public method Use(): int => tag + r.Id(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(5, new Res(7)); end; "
        "  return Res._n; "
        "end Main; end C;"), 1);
}

// §22.10 (v0.180) — a *borrowed* (non-`owned`) class parameter captured in a
// body is not disposed by the capturing class; the caller's `using var` owner
// disposes it exactly once (counter → 1). A spurious capture-disposal here
// would double-free the still-owned caller object.
BEZEL_TEST(codegen_owned_primary_ctor_param_1543, borrow_param_not_double_freed_22_10) {
    // per §22.10 (v0.180) / §28.5 (borrow, not owned)
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kRes +
        "class Holder(r: Res) "
        "  public method Use(): int => r.Id(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  do "
        "    using var res := new Res(1); "
        "    using var h := new Holder(res); "
        "    var ignore := h.Use(); "
        "  end; "
        "  return Res._n; "
        "end Main; end C;"), 1);
}

// §11.1 / §22.10 (v0.180) — the ResourceHolder pattern: an `owned` parameter
// consumed by an explicit `owned` field initializer is disposed by THAT field,
// not by a capture slot (the parameter is not body-referenced). Disposal count
// is exactly 1 — a capture-slot disposal on top would double-free.
BEZEL_TEST(codegen_owned_primary_ctor_param_1543, resourceholder_initializer_consume_no_double_free_22_10) {
    // per §11.1 ResourceHolder / §22.10 (v0.180)
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kRes +
        "class Holder(owned r: Res) "
        "  private owned _s: Res := r; "
        "end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(new Res(1)); end; "
        "  return Res._n; "
        "end Main; end C;"), 1);
}

// §28.2 / §22.10 (v0.180) — the generated Dispose is idempotent: an explicit
// `Dispose()` (twice) disposes the captured `owned` parameter exactly once
// (counter → 1), never re-running the destructor.
BEZEL_TEST(codegen_owned_primary_ctor_param_1543, capture_dispose_idempotent_22_10) {
    // per §28.2 (run-once) / §22.10 (v0.180)
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kRes +
        "class Holder(owned r: Res) "
        "  public method Use(): int => r.Id(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(new Res(1)); "
        "  h.Dispose(); "
        "  h.Dispose(); "
        "  return Res._n; "
        "end Main; end C;"), 1);
}

// §22.10 (v0.180) — an interface-typed `owned` primary-ctor parameter captured
// in a body is disposed through runtime dispatch (§28.5) to the underlying
// class's Dispose (counter → 1).
BEZEL_TEST(codegen_owned_primary_ctor_param_1543, interface_typed_owned_param_disposed_22_10) {
    // per §22.10 (v0.180) / §28.5 (interface-typed owned field)
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IThing method Id(): int; end IThing; "
        "class Res : IThing "
        "  public static _n: int := 0; "
        "  private _id: int; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  public method Id(): int => _id; "
        "  destructor _n := _n + 1; end destructor; "
        "end Res; "
        "class Holder(owned t: IThing) "
        "  public method Use(): int => t.Id(); "
        "end Holder; "
        "class C public static method Main(): int "
        "  do using var h := new Holder(new Res(9)); end; "
        "  return Res._n; "
        "end Main; end C;"), 1);
}
