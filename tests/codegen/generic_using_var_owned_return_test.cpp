// Codegen — discovery sweep #6: `using var x := Make()` inside a generic class's
// instance method, where `Make(): owned T` returns a fresh owned T, failed to
// dispose x at scope exit — the owned T leaked.
//
// ROOT CAUSE: usingVarOwnsClassObjectDynamically's owned-return branch gated on
// isClassOrInterfaceRef(initializer type), which is false for a bare generic
// type parameter T (declaration == null). The fix also accepts the case where T
// resolves under the active substitution (resolveTypeParamRef) to a class /
// interface — true in the per-instantiation specialization where the disposal is
// registered. §28.10 / §14.2.
//
// Controls (already correct, verified in the sweep): `using var x := new T()`
// and an explicit `x.Dispose()` both dispose correctly; only the
// owned-method-return + generic-T combination leaked.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// The verified repro: a single `using var x := Make()` disposes x.
BEZEL_TEST(generic_using_var_owned_return, owned_t_return_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> "
        "  public constructor() end constructor; "
        "  public method Make(): owned T return new T(); end Make; "
        "  public method MakeAndDiscard() using var x := Make(); end MakeAndDiscard; "
        "end G; "
        "class C public static method Main(): int "
        "  using var g := new G<Cell>(); "
        "  g.MakeAndDiscard(); "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// Multiple owned-returns in one scope all dispose (no leak, no double-free).
BEZEL_TEST(generic_using_var_owned_return, multiple_owned_returns) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> "
        "  public constructor() end constructor; "
        "  public method Make(): owned T return new T(); end Make; "
        "  public method Loop() using var a := Make(); using var b := Make(); end Loop; "
        "end G; "
        "class C public static method Main(): int "
        "  using var g := new G<Cell>(); "
        "  g.Loop(); g.Loop(); "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}
