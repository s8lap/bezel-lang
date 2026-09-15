// Spec-derived regression test for issue #3513 — a §18.5 / §28.3 string
// interpolation leak: when an expression part `$"{<expr>}"` contains a
// throwable call and the call throws, the partially-built string accumulator
// is abandoned.
//
// The interpolation loop in `lowerInterpolatedString`
// (LlvmBackend_Exprs.cpp) builds the result incrementally:
//   acc := emitStringConcat(acc, pv)   // produces a fresh +1 string
//   emitStringRelease(prev)            // releases the prior accumulator
// When `<expr>` contains a throwable call and it throws, `lowerExpr`
// branches to `unwindBB` via `emitThrowValue` — but `acc` was not registered
// in any disposal scope and is abandoned. The bz.string header (32 B) and
// the literal prefix buffer both leak, unboundedly in a loop.
//
// THE FIX (§18.5 / §28.3, v0.298): before calling `lowerExpr(*part.expr)`,
// push a disposal scope and register the current accumulator in it with
// `unwindOnly = true` / `stringRelease = true`. If `lowerExpr` throws,
// `emitUnwindDisposals()` at the throw site fires the scope's disposal and
// reclaims `acc`. On the normal path, null the slot and pop the scope (the
// prior `acc` was concat'd into the new string and will be released by
// `appendPart`). This mirrors the established `FfiExprTemp` /
// `AwaitedTaskTemp` pattern for other mid-expression temporaries.
//
// ASan + LSan on the generated program surfaces the leak as a "Direct leak
// of N byte(s)" for the bz.string header. `Res.live > 0` (leak) vs
// `Res.live == 0` (clean disposal) is the own-code oracle.
//
// per §18.5, §28.3, v0.298, #3513

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// ASan + LSan on the generated program: a leak, double-free or UAF surfaces
// as ran=false (-999); otherwise the program's exit code.
static int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

const char* kPrelude =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public constructor() Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; "
    "class MyErr: Exception end MyErr; "
    "class C "
    "  public static method Boom(): int "
    "    using var r := new Res(); "
    "    throw new MyErr(); "
    "  end Boom; ";

}  // namespace

// ===========================================================================
// Row 1 (FAILS-WITHOUT / PASSES-WITH): the measured #3513 case — an
// interpolation `$"{<expr>}"` where `<expr>` is a throwable call that
// constructs a Res before throwing. On the pre-fix compiler: the
// interpolation's accumulator (the bz.string header for "abcdefgh=") is
// abandoned on the exception edge, and ASan reports it as a leak (exit code
// 1 from ASan's leak detection, not the program's 0). On the post-fix
// compiler: the accumulator is registered for unwind-only disposal before
// `lowerExpr` is called; the throw branches to `unwindBB`,
// `emitUnwindDisposals()` fires the disposal and reclaims the accumulator;
// ASan sees no leak and the program exits 0. FAILS WITHOUT / PASSES WITH.
// per §18.5, §28.3, v0.298, #3513
BEZEL_TEST(interp_throw_leak_3513,
           interpolation_expr_throws_accumulator_leaks_without_fix) {
    BEZEL_EXPECT_EQ(run(std::string(kPrelude) +
        "  public static method Main(): int "
        "    for var i in 1..10 do "
        "      try "
        "        _ := $\"abcdefgh={C.Boom()}\"; "
        "      catch var e: MyErr => do end; "
        "      end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}
