// fix #2886 (§23.3 / §27.3 / §28.3, v0.240): a lambda emitted INSIDE an
// INLINED block-bodied property setter must not inherit the setter's
// `inlinedAccessorEnd_` accessor-end block. That block lives in the
// enclosing (caller) function; a bare `return;` lowered inside the lambda
// body (LlvmBackend_Stmts.cpp ~1719) was branching to it via
// `builder_.CreateBr(inlinedAccessorEnd_)`, producing a cross-function
// `br label %setter.end` that the LLVM module verifier rejects with
// "Referring to a basic block in another function!".
//
// The reproducer in the issue is the test: single module, an inlined
// block-bodied property setter whose body defines a `() -> ()` lambda
// that carries a bare `return;`. Pre-fix the IR verifier aborts the
// compile and `ran` stays false. Post-fix the program runs to completion
// and returns 117 (= 100 + 1 * 10 + 7).
//
// Cites the same family as #2872 (a separate `disposerEntryExc_` leakage
// from the same `NestedBodyScope` under-population): both are per-function
// Emitter members nulled by `NestedBodyScope` for the three accessor body
// emitters but NOT for `lowerLambda`, which had to be repaired directly
// here because it does NOT use `NestedBodyScope` (it manages its own
// sm*/async*/unwind stack).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §23.3 / #2886: a block-bodied property setter `K` holds a block-bodied
// lambda `f` whose body contains a bare `return;`. Pre-fix this lowered
// with `inlinedAccessorEnd_` still wired to the setter's accessor-end
// block (a BasicBlock* in the enclosing function), so the lambda's
// `return;` lowered as `br label %setter.end` — a cross-function IR
// reference the verifier rejects. Post-fix `lowerLambda` saves/clears
// `inlinedAccessorEnd_` like the sibling per-function members
// (`unwindTarget_`, `propagateBB_`, `currentCaughtExc_`), so the lambda
// body's `return;` falls through to the ordinary void-return path of
// the lambda's own function.
//
// Exit: 100 + Holder.Trace * 10 + h.K = 100 + 1*10 + 7 = 117.
BEZEL_TEST(codegen_lambda_inlined_setter_2886,
           lambda_inside_inlined_block_bodied_property_setter) {
    BEZEL_EXPECT_EQ(runFull(
        "module Main; "
        "public class Holder "
        "  public static Trace: int; "
        "  private _k: int; "
        "  public property K: int "
        "    get => do return _k; end; "
        "    set => do "
        "      var f: () -> () := lambda() => do "
        "        Holder.Trace := Holder.Trace + 1; "
        "        return; "
        "      end; "
        "      f(); "
        "      _k := value; "
        "      return; "
        "    end; "
        "  end K; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    h.K := 7; "
        "    return 100 + Holder.Trace * 10 + h.K; "
        "  end Main; "
        "end C;"), 117);
}
