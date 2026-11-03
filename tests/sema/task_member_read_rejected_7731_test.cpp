// Spec-derived regression test for issue #7731 -- a read of a member `Task`
// DOES NOT HAVE was accepted untyped, and the read degraded at codegen.
//
// THE DEFECT.
//
// `await x.y` parses as `await (x.y)` -- primary access binds tighter than
// unary `await` -- so `.Length` is read OFF THE TASK. spec §30.3 (v0.350)
// gives `Task` / `Task<T>` / `OwningTask<T>` only IsCompleted, IsFaulted,
// IsCancelled, Result and Wait(). The compiler ACCEPTED the read untyped; the
// initializer or the named argument then degraded at codegen (an `internal:`
// warning, exit 0, and the argument's side effect NEVER TRACED), and the
// untyped local surfaced as a location-less `+` error.
//
// THE ISSUE'S OWN REPRO.
//
//   length := await SlowS(..).Length
//   var n := await SlowS(..).Length
//
// WHAT FIXED IT.
//
// The defect was fixed by commit c1225d9c (the #7731 lane's commit, "reject a
// read of a member `Task` does not have"). Its subject's issue prefix is left
// out of this message so that this message closes #7731 and nothing else;
// the commit's own subject carries no closing keyword, so the issue stayed
// OPEN until this commit.
//
// THE DOCUMENTS.
//
// spec §30.3 (v0.350) gives the closed member surface of `Task`; spec §17.1
// (v0.350) gives the binding: "`await x.y` parses as `await (x.y)`". spec
// §22.6 (v0.350): a call or a member read that matches no declared member
// is an error, reported at the read.
//
// THE ORACLE.
//
// The rejection rows assert the CORRECT observable: the program does NOT
// run, and the driver's diagnostic names the member that does not exist and
// cites the section that fixes the surface. The control rows assert the
// members stdlib §6.7 / spec §30.3 DO declare -- IsCompleted and Result --
// still work, so a fix that closed the whole surface would not pass.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Threading.Tasks; "
           "public class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The rejection: the program did not run, the driver rejected it at a REAL
// source location (never a crash / signal / timeout), and the diagnostic
// names the member that does not exist and cites the closed surface.
void expectMemberRefused(const JitResult& r, const char* member) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, member));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "§30.3"));
}

}  // namespace

// ===========================================================================
// THE PIN ROW -- THE READ OF A MEMBER `Task` DOES NOT HAVE.
//
// `Task.FromResult(5)` is a `Task<int>`; `Length` is not one of its
// members (spec §30.3), so the read is an error at the read.
// ===========================================================================
BEZEL_TEST(task_member_read_rejected_7731,
           task_member_that_does_not_exist_is_refused_30_3) {
    auto r = runAot(prog(
        "    using var t := Task.FromResult(5); "
        "    var n := t.Length; "
        "    Console.WriteLine(n); "));
    expectMemberRefused(r, "Length");
}

// ===========================================================================
// THE PIN ROW -- THE ISSUE'S OWN SPELLING, WITH THE `await`.
//
// spec §17.1: `await x.y` parses as `await (x.y)`, so the member is read off
// the Task and the `await` never gets to it. The read is the same refusal.
// ===========================================================================
BEZEL_TEST(task_member_read_rejected_7731,
           await_of_a_task_member_that_does_not_exist_is_refused_30_3) {
    auto r = runAot(
        "module M; import System; import System.Threading.Tasks; "
        "public class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Task.FromResult(5); "
        "    var n := await t.Length; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectMemberRefused(r, "Length");
}

// ===========================================================================
// THE CONTROL -- THE MEMBERS THE SURFACE DOES DECLARE.
//
// spec §30.3 / stdlib §6.7: `Task` declares IsCompleted, IsFaulted,
// IsCancelled, Result and Wait(). The rows below must be green on both
// arms, so that the rejection rows cannot be satisfied by "the whole surface
// was closed".
// ===========================================================================
BEZEL_TEST(task_member_read_rejected_7731,
           declared_task_members_still_work_30_3) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var t := Task.FromResult(5); "
            "    Console.WriteLine(t.IsCompleted.ToString()); "
            "    Console.WriteLine(t.Result); ")),
        std::string("True\n5"));
}
