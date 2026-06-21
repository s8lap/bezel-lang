// Codegen — issue #817: a method of a GENERIC class that returns a CONCRETE
// (non-type-parameter) type while reading a generic-typed field emitted
// malformed IR (§14.2 monomorphization).
//
// THE BUG: a generic class's method bodies are emitted twice — (1) a correct
// per-instantiation specialization, used at every call (the receiver always
// binds the concrete type-arg substitution), and (2) an unspecialized TEMPLATE
// emitted only so the module verifies. The template lowers `T`-typed
// fields/locals at the i32 / `{i8,i32}` placeholder layout. When the method's
// SIGNATURE is concrete — e.g. `method GetConcrete(): Counter?` returning a
// `T?` field — the template's placeholder body (`{i8,i32}`) disagreed with the
// concrete return type (`ptr`), so the whole module failed IR verification:
//   "Function return type does not match operand type of return inst!
//    ret { i8, i32 } %2 / ptr".
//
// THE FIX: emitFunctionBody emits a type-safe stub (`ret null-of-return-type`)
// for a generic-class / generic-method TEMPLATE instead of the real, possibly
// ill-typed body. The template is dead code; every call routes to a
// specialization. The third test pins the key soundness property — a
// concrete-return method that ignores `T` still runs its REAL body (via the
// specialization), not the stub.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// The verified repro: a concrete nullable class return reading a `T?` field.
BEZEL_TEST(generic_concrete_return_817, nullable_concrete_return_from_t_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public id: int; "
        "  public constructor(i: int) id:=i; end constructor; "
        "  destructor end destructor; end Counter; "
        "class Holder<T> "
        "  private owned _val: T?; "
        "  public constructor(owned v: T) _val:=v; end constructor; "
        "  destructor end destructor; "
        "  public method GetConcrete(): Counter? => _val; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<Counter>(new Counter(7)); "
        "  var c := h.GetConcrete(); "
        "  return c!.id; "  // 7
        "end Main; end C;"), 7);
}

// The same class also returning T? (a true type-parameter return) must keep
// working — the specialization path is unaffected by the template stub.
BEZEL_TEST(generic_concrete_return_817, t_field_round_trip_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public id: int; "
        "  public constructor(i: int) id:=i; end constructor; "
        "  destructor end destructor; end Counter; "
        "class Holder<T> "
        "  private owned _val: T?; "
        "  public constructor(owned v: T) _val:=v; end constructor; "
        "  destructor end destructor; "
        "  public method GetT(): T? => _val; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<Counter>(new Counter(9)); "
        "  var g := h.GetT(); "
        "  return g!.id; "  // 9
        "end Main; end C;"), 9);
}

// Soundness pin: a concrete-int method that ignores T must run its REAL body
// (42 via the specialization), NOT the dead template stub (which would return 0).
BEZEL_TEST(generic_concrete_return_817, concrete_method_ignoring_t_runs_real_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter public id: int; "
        "  public constructor(i: int) id:=i; end constructor; "
        "  destructor end destructor; end Counter; "
        "class Holder<T> "
        "  private owned _val: T?; "
        "  public constructor(owned v: T) _val:=v; end constructor; "
        "  destructor end destructor; "
        "  public method Tag(): int => 42; "
        "  public method GetT(): T? => _val; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<Counter>(new Counter(5)); "
        "  return h.Tag() + h.GetT()!.id; "  // 42 + 5 = 47
        "end Main; end C;"), 47);
}
