// fix §35.3 / §14.1 (v0.334): a payload-free case access on a CONCRETE
// generic instantiation (`Res<int>.Bad`) materialized the ERASED
// `%choice.Res`, while the type-annotation side of the very same choice
// produced the SPECIALIZED `%choice.Res$spec0`. The two are structurally
// identical but NOMINALLY DISTINCT LLVM types, so any boundary between them
// produced malformed IR that `verifyModule` rejected at the end of codegen —
// a total compile failure, not a wrong answer:
//
//   define i32 @"bz$M.C$Describe"(%"choice.Res$spec0" %0)      <- callee
//     %0 = call i32 @"bz$M.C$Describe"(%choice.Res { i32 1, … })   <- erased
//
//   "Call parameter type does not match function signature!"          (arg)
//   "Function return type does not match operand type of return inst!" (result)
//
// Root cause (src/codegen/LlvmBackend_Setup.cpp, `choiceCaseConstant`): the
// payload-free case form calls `choiceStruct(choiceTy)` with no substitution
// active, so it took the unspecialized arm. The PAYLOAD-carrying form —
// `Res<int>.Ok(5)` — is lowered by `lowerCall`, which installs the same
// substitution via `RecordSubstGuard` (`choiceCtorSubst`), which is why only
// the payload-free form aborted. `lowerTypeRef`'s concrete-choice branch is the
// single place that decides the specialized-vs-erased spelling, so the fix
// routes the case value through it (its `allUnbound` guard keeps an
// unspecialized generic template body on the base struct — the twin of the
// record defect the same branch records at `%record.Box$spec0` vs
// `%record.Box`).
//
// Oracle: runAotChecked — the strict AOT path (fork-isolated Driver::compileFile
// + clang link + native exec) under generated-code ASan/LSan. `emitObjectFile`
// runs `verifyModule`, so a regression re-fails the compile rather than the
// comparison. The JIT harness is deprecated (it skips verifyModule entirely and
// would have passed on this bug's pre-fix code).
#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The generic choice under test: one payload-carrying case and two
// payload-free ones, so the specialization is observable (the payload slot
// layout depends on the type argument) AND every case is reachable.
constexpr const char* kChoice =
    "choice Res<T> Ok(v: T), Bad, Err; end Res; ";

}  // namespace

// ── Row 1 — THE filed repro: a payload-free case value passed as the argument
// to a method specialized on the concrete choice `Res<int>`. Pre-fix the callee
// signature was `%choice.Res$spec0` and the argument `%choice.Res`:
// verifyModule aborted the whole module, so the program never linked.
//
// The rows below assert the program's ANSWER as well as its survival, so a
// future change that makes the two types agree by silently erasing BOTH sides
// (rather than specializing both) cannot pass.
BEZEL_TEST(choice_nullary_case_generic_specialization,
           payload_free_case_passed_to_method_specialized_35_3) {
    std::string src =
        "module M1; " + std::string(kChoice) +
        "class C "
        "  private static method Score(r: Res<int>): int "
        "    return (case r of Bad => 1, Err => 2, Ok(var v) => v + 100); "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Res<int>.Bad)); "
        "    Console.WriteLine(Score(Res<int>.Err)); "
        "    Console.WriteLine(Score(Res<int>.Ok(7))); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n2\n107\n"));
}

// ── Row 2 — the SECOND boundary the same defect broke: a payload-free case
// value RETURNED from a method whose declared result is the concrete
// instantiation `Res<int>` ("Function return type does not match operand type
// of return inst!" pre-fix). A distinct lowering site (the return inst, not the
// call's argument), so it pins that the fix covers both, not just the one the
// filed repro reached.
BEZEL_TEST(choice_nullary_case_generic_specialization,
           payload_free_case_returned_from_generic_result_method_35_3) {
    std::string src =
        "module M2; " + std::string(kChoice) +
        "class C "
        "  public static method Make(): Res<int> "
        "    return Res<int>.Bad; "
        "  end Make; "
        "  public static method Main(): int "
        "    var x := Make(); "
        "    Console.WriteLine((case x of Bad => 1, Err => 2, Ok(var v) => v + 100)); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// ── Row 3 — CONTROL, and the falsification guard for the fix's own rule. The
// payload-carrying form already worked pre-fix (it goes through `lowerCall`'s
// `RecordSubstGuard`), so it must still work — this row fails if the change
// disturbed the construction path.
//
// It also pins the DISCRIMINATOR the diagnosis rests on: the failing shape is
// the payload-FREE form, and the two agree only because `lowerTypeRef` gives
// both the same specialized spelling.
BEZEL_TEST(choice_nullary_case_generic_specialization,
           control_payload_case_still_specializes_35_3) {
    std::string src =
        "module M3; " + std::string(kChoice) +
        "class C "
        "  private static method Score(r: Res<int>): int "
        "    return (case r of Bad => 1, Err => 2, Ok(var v) => v + 100); "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Res<int>.Ok(7))); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("107\n"));
}

// ── Row 4 — the DISCRIMINATOR itself: the payload-carrying case value is
// passed INLINE at the same call, in the same program, to the same specialized
// method as Row 1. Pre-fix this single line compiled (via `lowerCall`'s
// `RecordSubstGuard`) while the payload-free line beside it aborted, so the
// pair is the differential control: if a future change makes BOTH fail, Row 4
// catches the regression that Row 1 alone would misattribute.
BEZEL_TEST(choice_nullary_case_generic_specialization,
           inline_payload_case_at_same_call_site_differential_control_35_3) {
    std::string src =
        "module M4; " + std::string(kChoice) +
        "class C "
        "  private static method Score(r: Res<int>): int "
        "    return 1; "
        "  end Score; "
        "  public static method Main(): int "
        "    Console.WriteLine(Score(Res<int>.Bad)); "
        "    Console.WriteLine(Score(Res<int>.Ok(7))); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n1\n"));
}

// ── Row 5 — a payload-free case read back as a LOCAL of the concrete
// instantiation, with NO method in the picture. This is the shape that already
// worked pre-fix (the local's annotation lowers through `lowerTypeRef`, and the
// unspecialized `choiceStruct` arm agreed with it), so it is the control that
// says the fix did not OVER-reach: it must still compile and read back the
// right tag.
BEZEL_TEST(choice_nullary_case_generic_specialization,
           control_payload_free_case_in_concrete_local_35_3) {
    std::string src =
        "module M5; " + std::string(kChoice) +
        "class C public static method Main(): int "
        "  var x: Res<int> := Res<int>.Bad; "
        "  Console.WriteLine((case x of Bad => 1, Err => 2, Ok(var v) => v + 100)); "
        "  return 0; "
        "end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}