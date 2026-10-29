// The ASYNC spelling of the #7415 copy-adoption: `await using var x := src;`
// over an OWNED move-only PARAMETER of an `async` method must MOVE the value,
// leaving the parameter moved-from (issue #7487, a coverage pin for the #7415
// fix; spec §28.10 / §28.12 / §15.3 / §30.1 / §35.4, v0.350).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md §28.10 (v0.350) — the move-only adoption
//   paragraph: the unmarked copy of a move-only local or parameter ADOPTS —
//   "`y` carries the value's owned slots to the end of *its* block and `x` is
//   left moved-from" — and `await using var` is the same adoption (§28.10's
//   *Adopting a named owned binding* lists "`await using var y := x;`" with the
//   `using var` forms).
//
//   docs/Bezel_Language_Spec.md §28.12 (v0.350) — the scope-end discharge of
//   an `await using var` binding is a rule DISJOINT from the adoption rule;
//   both apply at an `await using` over an owned move-only parameter.
//
//   docs/Bezel_Language_Spec.md §30.1 (v0.350) — an `owned` parameter of an
//   async method transfers into the frame; one never moved out is disposed by
//   the frame at its terminal state. The adoption IS a move out, so the frame
//   must dispose nothing and the binding disposes exactly once.
//
// IS THE ASYNC SPELLING A DIFFERENT CODE PATH? NO for the move, YES for the
// frame. The COPY's move is the SAME code path as `using var`: the LocalDecl
// lowering in LlvmBackend_Stmts.cpp calls `markOwningChoiceLocalMovedFrom(ld,
// ld->initializer)` for every `LocalDecl`, `isAwaitUsing` included, and the sema
// half is the same `noteOwningChoiceWholeMoveRead`. What the async spelling adds
// is the frame: Ownership.cpp's `sawYield_ || isAsync_` arm decides, from
// `everDischarged_`, whether the parameter is flagged `iteratorAutoDispose` and
// carries the "implicitly disposed by the frame at teardown" warning.
//
// WHAT EACH ROW CAN FAIL ON (measured by disabling #7415's two halves):
//   * the three disposal-COUNT rows are CONTROL rows. They stay green with the
//     codegen zero-store disabled, with the sema parameter seed disabled, and
//     with BOTH disabled, because an owning-choice parameter of an async method
//     is not disposed by the frame at all today (the warning names a disposal
//     that does not run; see the handback of lane C1-L3 for the sibling). They
//     pin the observable move semantics (one DTOR, in the callee) so a future
//     frame disposal of this parameter cannot double it.
//   * the read-after-adoption row is a REAL pin of the sema half: it fails when
//     the parameter seed in `noteOwningChoiceWholeMoveRead` is disabled.
//   * the diagnostic half — the frame-teardown warning must be ABSENT for the
//     copied parameter and PRESENT for an un-copied one — is a real pin in the
//     lit track, because `runAot` fills diagnostics only on reject:
//     tests/lit/ownership/await_using_copy_owned_param_7487__no_frame_teardown_warn.bzl
//
// THE ORACLE. Disposal COUNT (a printing / counting destructor), plus the
// specific "was already moved out" diagnostic for the read after the adoption.
//
// per §28.10, §28.12, §15.3, §30.1, §35.4, v0.350

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kNamedDecls =
    "class Res "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"DTOR\"); end destructor; "
    "end Res; "
    "choice OChoice Text(owned r: Res), Empty; end OChoice; ";

const char* kCountingDecls =
    "class Res "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res._n := Res._n + 1; end destructor; "
    "end Res; "
    "choice OChoice Text(owned r: Res), Empty; end OChoice; ";

std::string prog(const char* decls, const std::string& members) {
    return std::string("module M; ") + decls + " class Program " + members +
           " end Program;";
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

BEZEL_TEST(ownership_await_using_copy_owned_param_7487,
           spec_28_10_await_using_copy_of_an_owned_owning_choice_param_moves_it) {
    // The #7415 repro in its async spelling. One disposal, in the callee: the
    // `await using` binding carries the payload, the parameter is moved-from,
    // and the frame's terminal-state disposal (§30.1) finds nothing to do.
    expectRunOutput(
        runAot(prog(kNamedDecls,
                    "public static async method Take(owned src: OChoice): Task "
                    "await using var x := src; "
                    "end Take; "
                    "public static async method Main(): Task "
                    "var osrc := new Res(); "
                    "var src := OChoice.Text(move osrc); "
                    "await Take(move src); "
                    "Console.WriteLine(\"END\"); "
                    "end Main; ")),
        std::string("DTOR\nEND\n"));
}

BEZEL_TEST(ownership_await_using_copy_owned_param_7487,
           spec_28_10_await_using_move_spelled_copy_of_an_owned_param_moves_it) {
    // The `move` spelling of the same adoption (§28.10 *Adopting a named owned
    // binding*: `y := move x`).
    expectRunOutput(
        runAot(prog(kNamedDecls,
                    "public static async method Take(owned src: OChoice): Task "
                    "await using var x := move src; "
                    "end Take; "
                    "public static async method Main(): Task "
                    "var osrc := new Res(); "
                    "var src := OChoice.Text(move osrc); "
                    "await Take(move src); "
                    "Console.WriteLine(\"END\"); "
                    "end Main; ")),
        std::string("DTOR\nEND\n"));
}

BEZEL_TEST(ownership_await_using_copy_owned_param_7487,
           spec_30_1_every_awaited_copy_in_a_loop_is_disposed_exactly_once) {
    // 500 awaited calls: exactly 500 disposals. Above is the frame plus the
    // binding both disposing; below is a suppression grown into a leak.
    expectRunOutput(
        runAot(prog(kCountingDecls,
                    "public static async method Take(owned src: OChoice): Task "
                    "await using var x := src; "
                    "end Take; "
                    "public static async method Main(): Task "
                    "var i := 0; "
                    "while i < 500 do "
                    "var osrc := new Res(); "
                    "var src := OChoice.Text(move osrc); "
                    "await Take(move src); "
                    "i := i + 1; "
                    "end while; "
                    "Console.WriteLine(Res._n); "
                    "end Main; ")),
        std::string("500\n"));
}

BEZEL_TEST(ownership_await_using_copy_owned_param_7487,
           spec_15_3_a_moved_from_async_owning_choice_parameter_may_not_be_read) {
    // §15.3: "Reading from a moved-from location is a compile error." The
    // `await using` adoption left the parameter moved-from, so the read after
    // it is the error — not a runtime fault. Pre-#7415 this compiled cleanly.
    const JitResult r = runAot(prog(
        kNamedDecls,
        "public static async method Take(owned src: OChoice): Task "
        "await using var x := src; "
        "using var z := src; "
        "end Take; "
        "public static async method Main(): Task "
        "var osrc := new Res(); "
        "var src := OChoice.Text(move osrc); "
        "await Take(move src); "
        "end Main; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "was already moved out"));
}
