// The `using var` COPY of an OWNED parameter whose declared type is the
// NULLABLE WRAPPER of a move-only value type (`OChoice?`) must MOVE the value,
// leaving the SOURCE moved-from (issue #7486, a coverage pin for the #7415
// fix; spec §28.10 / §15.3 / §35.9 / §35.4, v0.350).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md §28.10 (v0.350) — "Where `x` is a local of
//   **move-only value type** (§15.3) the unmarked form **adopts**: a move-only
//   binding has no borrow reading to disambiguate, so the plain read moves
//   (K3.12; §15.3's *Local-to-local assignment*), `y` carries the value's owned
//   slots to the end of *its* block and `x` is left moved-from."
//
//   docs/Bezel_Language_Spec.md §15.3 (v0.350) — *Parameter passing*: "for a
//   move-only V, it is by value-move (transfer ownership). … A parameter
//   declared `(owned x: V)` is identical to plain `(x: V)` for a move-only V".
//
//   docs/Bezel_Language_Spec.md §35.9 (v0.350) — `C?` for an owning choice `C`
//   is the nullable VALUE wrapper: `{ hasValue, <choice struct> }`, move-only
//   exactly as `C` is.
//
// WHY A SEPARATE FILE. #7415's fix reaches a parameter source in
// `markOwningChoiceMovedFromCore` (LlvmBackend_Disposal.cpp). That function
// derives the storage type it zero-stores into in TWO branches: the bare choice
// (`choiceStruct`) and the nullable wrapper (`nullableValueStructTy`). The
// eight #7415 rows run only through the first. Without a row through the second
// the spec sentence "the copy of a move-only parameter moves it" is pinned for
// one spelling of the type and merely assumed for the other.
//
// THE ORACLE. Every row pins the DISPOSAL COUNT (a printing / counting
// destructor): a doubled disposal and a suppressed one are distinguishable from
// each other and from the spec's answer. The read-after-adoption row pins the
// SPECIFIC diagnostic, because the compiler ACCEPTED that program before
// #7415's sema half.
//
// per §28.10, §15.3, §35.9, §35.4, v0.350

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

// The CALLEE under test: the nullable-wrapper parameter, copied by `copy`.
std::string callee(const std::string& copy) {
    return "public static method Take(owned src: OChoice?) " + copy +
           " end Take; ";
}

// The CALLER: builds the wrapped value, hands it over with `move`.
const char* kCaller =
    "public static method Main() "
    "var osrc := new Res(); "
    "var s: OChoice? := OChoice.Text(move osrc); "
    "Take(move s); "
    "Console.WriteLine(\"END\"); "
    "end Main; ";

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

BEZEL_TEST(ownership_using_var_copy_nullable_param_7486,
           spec_28_10_using_var_copy_of_a_nullable_owning_choice_param_moves_it) {
    // The #7415 repro with the parameter's type widened to the nullable
    // wrapper. §28.10: the copy MOVES, so the payload is disposed exactly once
    // (by the copy) and the parameter's own scope-end pass disposes nothing.
    // Disabling the nullable branch of the zero-store leaves both the copy and
    // the parameter holding the payload: two disposals, the second a
    // heap-use-after-free.
    expectRunOutput(
        runAot(prog(kNamedDecls,
                    callee("using var x := src;") + kCaller)),
        std::string("DTOR\nEND\n"));
}

BEZEL_TEST(ownership_using_var_copy_nullable_param_7486,
           spec_15_3_var_binding_of_a_nullable_owning_choice_param_moves_it) {
    // §15.3 *Local-to-local assignment*: `var y := x;` is the move for a
    // move-only V. Same transfer as the `using var` form, same count.
    expectRunOutput(
        runAot(prog(kNamedDecls, callee("var x := src;") + kCaller)),
        std::string("DTOR\nEND\n"));
}

BEZEL_TEST(ownership_using_var_copy_nullable_param_7486,
           spec_15_3_move_spelled_copy_of_a_nullable_owning_choice_param_moves_it) {
    // The `move` spelling is the same transfer (§15.3 *Local-to-local
    // assignment* scope note).
    expectRunOutput(
        runAot(prog(kNamedDecls, callee("using var x := move src;") + kCaller)),
        std::string("DTOR\nEND\n"));
}

BEZEL_TEST(ownership_using_var_copy_nullable_param_7486,
           spec_15_3_every_copied_nullable_payload_in_a_loop_is_disposed_exactly_once) {
    // 500 iterations through the nullable branch: exactly 500 disposals. Above
    // is the double disposal, below is a suppression grown into a leak.
    expectRunOutput(
        runAot(prog(kCountingDecls,
                    callee("using var x := src;") +
                    "public static method Churn() "
                    "var i := 0; "
                    "while i < 500 do "
                    "var osrc := new Res(); "
                    "var s: OChoice? := OChoice.Text(move osrc); "
                    "Take(move s); "
                    "i := i + 1; "
                    "end while; "
                    "end Churn; "
                    "public static method Main() "
                    "Churn(); "
                    "Console.WriteLine(Res._n); "
                    "end Main; ")),
        std::string("500\n"));
}

BEZEL_TEST(ownership_using_var_copy_nullable_param_7486,
           spec_35_9_copy_of_an_absent_nullable_param_disposes_nothing) {
    // CONTROL row (cannot fail on the arm alone: an absent value has no
    // payload to double-free). It pins that the zero-store over an absent
    // wrapper (`hasValue == 0`) is harmless and that nothing is disposed:
    // no DTOR line, the program runs to END.
    expectRunOutput(
        runAot(prog(kNamedDecls,
                    callee("using var x := src;") +
                    "public static method Main() "
                    "var n: OChoice? := null; "
                    "Take(move n); "
                    "Console.WriteLine(\"END\"); "
                    "end Main; ")),
        std::string("END\n"));
}

BEZEL_TEST(ownership_using_var_copy_nullable_param_7486,
           spec_15_3_a_moved_from_nullable_owning_choice_param_may_not_be_read) {
    // §15.3: "Reading from a moved-from location is a compile error." The
    // adoption left the nullable parameter moved-from, so a second adoption of
    // it is the error. Pre-#7415 the compiler accepted this program.
    const JitResult r = runAot(prog(
        kNamedDecls,
        callee("using var x := src; using var z := src;") + kCaller));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "was already moved out"));
}
