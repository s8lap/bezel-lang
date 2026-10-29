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

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// The verified repro: a single `using var x := Make()` disposes x.
// §14.1 (#992): `new T()` requires `where T: new()` — added to G<T>.
BEZEL_TEST(generic_using_var_owned_return, owned_t_return_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> where T: new() "
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
// §14.1 (#992): `new T()` requires `where T: new()` — added to G<T>.
BEZEL_TEST(generic_using_var_owned_return, multiple_owned_returns) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class G<T> where T: new() "
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

// #1274 (§28.10): a `using var` initialized from a BORROW — a bare non-`owned`
// parameter — must NOT assume a disposal obligation. Before the fix, `using var
// c := x` inside a method taking a borrow `x` disposed the borrowed object at the
// method's scope end, and the caller's `using var` disposed it again → double-free
// (SIGSEGV / a negative live-count). Now the borrow-param using-var registers no
// disposal; the owner disposes exactly once. Cell.n (live count) returns to 0.
BEZEL_TEST(generic_using_var_owned_return, using_var_of_borrow_param_no_double_free_1274) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Prog "
        "  public static method Take(x: Cell) using var c := x; end Take; "
        "  public static method Use(): int "
        "    do "
        "      using var w := new Cell(); "  // n = 1
        "      Prog.Take(w); "               // borrow — Take must NOT dispose
        "    end; "                          // w disposes once → n = 0
        "    return Cell.n; "                // 0 (disposed exactly once, no double-free)
        "  end Use; "
        "end Prog; "
        "class C public static method Main(): int return Prog.Use(); end Main; end C;"),
        0);
}

// #1274 control: a `using var` initialized from an `owned` parameter is a genuine
// ownership transfer and MUST still dispose exactly once (the fix only excludes
// non-`owned` borrow parameters). Cell.n returns to 0 via the using-var disposal.
BEZEL_TEST(generic_using_var_owned_return, using_var_of_owned_param_still_disposes_1274) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class Prog "
        "  public static method Take(owned x: Cell) using var c := move x; end Take; "
        "  public static method Use(): int "
        "    do Prog.Take(new Cell()); end; "  // transfer → Take's using var disposes once
        "    return Cell.n; "                   // 0
        "  end Use; "
        "end Prog; "
        "class C public static method Main(): int return Prog.Use(); end Main; end C;"),
        0);
}
