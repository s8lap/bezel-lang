// Spec-derived regression tests for issue #6673: a write of `null` (or of a
// nullable value) inside a `try` block, or inside its `finally`, must
// invalidate a §7.4 narrowing for every path that reaches past it.
//
// THE RULE. Spec §7.4 (v0.339), "How narrowing is established": "a fact holds
// at a point only when it holds on every path that reaches it"; "How narrowing
// is invalidated": "the variable is reassigned from a nullable-typed
// expression; or the variable is passed as a `ref` or `out` argument". §31.2: a
// catch arm runs when the protected block was interrupted, at any point of it,
// and the `finally` runs on every exit of the block and of each arm.
//
// So after `s := null; P.Boom(true); s := "y";` inside the block, a catch arm
// (and anything after the `try` that a falling-through arm reaches) sees the
// state at the throw, where `s` is null. A `finally` that stores `null` leaves
// `s` nullable after the statement.
//
// WHAT WAS BROKEN. MEASURED on 2e900ab1 with the real `bezelc
// --sanitize=address`: every `_rejected_` row below compiled; 24 of the 30
// generated rejection rows segfaulted at run time (the 6 `try_null_finally_reads`
// rows compile and do not crash only because their block does not throw), and
// so did the seven hand-written rejection rows (`ref_arg_try_catch` through
// `break_through_finally`), each of which segfaulted. The
// catch-arm entry was the block's exit state intersected with the entry state,
// blind to an interior store a later store masks; the `finally` was walked in
// the last arm's exit state; and the after-`try` state was the `finally` exit
// UNIONED with the pre-`finally` join, which resurrected a fact the `finally`
// invalidated.
//
// Three spellings of the narrowing (`decl`: `var s: string? := "x"`, `asg`:
// `s := "x"`, `test`: `if s <> null then`) and two uses (`arg`: a non-nullable
// parameter, `len`: `.Length`) are crossed with each shape. The `_runs_` rows
// are controls that must stay accepted, with the stdout and exit code they
// produce. `catch_arm_null_then_rethrow` was rejected on 2e900ab1: an arm whose
// end is unreachable (here a rethrow) is not a path to the point after the
// `try`, so its exit state no longer joins there (§7.4 "every path that
// reaches it").

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

constexpr const char* kPrelude = R"bzl(module M;
static class S
  public static method F(x: string): int => x.Length;
end S;
public class P
  public static method MS(f: bool): string?
    if f then return "s"; end;
    return null;
  end MS;
  public static method Boom(f: bool): int
    if f then throw new InvalidOperationException("x"); end;
    return 1;
  end Boom;
  public static method Clear(ref v: string?)
    v := null;
  end Clear;
  public static method Log(m: string)
    Console.WriteLine(m);
  end Log;

)bzl";

constexpr const char* kTail = "end P;\n";

JitResult run(const char* body) {
    return runAot(std::string(kPrelude) + body + kTail);
}

const char* const kNullableArgDiag =
    "is nullable but the target type is non-nullable";
const char* const kNullableMemberDiag =
    "member access `.Length` on a nullable value requires narrowing";

void expectRejected(const JitResult& r, const char* diag) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, diag));
}

void expectRuns(const JitResult& r, int exitCode, const char* out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_throw_catch_decl_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_throw_catch_decl_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_throw_catch_asg_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_throw_catch_asg_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_throw_catch_test_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(true);
        s := "y";
      catch
        InvalidOperationException => P.Log("caught"),
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_throw_catch_test_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(true);
        s := "y";
      catch
        InvalidOperationException => P.Log("caught"),
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_decl_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(false);
    finally
      s := null;
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_decl_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(false);
    finally
      s := null;
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_asg_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      P.Boom(false);
    finally
      s := null;
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_asg_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      P.Boom(false);
    finally
      s := null;
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_test_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        P.Boom(false);
      finally
        s := null;
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_test_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        P.Boom(false);
      finally
        s := null;
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_finally_reads_decl_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    finally
      P.Log(s);
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_finally_reads_decl_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    finally
      P.Log(s);
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_finally_reads_asg_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    finally
      P.Log(s);
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_finally_reads_asg_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    finally
      P.Log(s);
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_finally_reads_test_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(false);
        s := "y";
      finally
        P.Log(s);
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_finally_reads_test_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(false);
        s := "y";
      finally
        P.Log(s);
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           nested_if_null_throw_catch_decl_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      if P.Boom(false) = 1 then
        s := null;
        P.Boom(true);
        s := "y";
      end if;
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           nested_if_null_throw_catch_decl_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      if P.Boom(false) = 1 then
        s := null;
        P.Boom(true);
        s := "y";
      end if;
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           nested_if_null_throw_catch_asg_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      if P.Boom(false) = 1 then
        s := null;
        P.Boom(true);
        s := "y";
      end if;
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           nested_if_null_throw_catch_asg_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      if P.Boom(false) = 1 then
        s := null;
        P.Boom(true);
        s := "y";
      end if;
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           nested_if_null_throw_catch_test_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        if P.Boom(false) = 1 then
          s := null;
          P.Boom(true);
          s := "y";
        end if;
      catch
        InvalidOperationException => P.Log("caught"),
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           nested_if_null_throw_catch_test_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        if P.Boom(false) = 1 then
          s := null;
          P.Boom(true);
          s := "y";
        end if;
      catch
        InvalidOperationException => P.Log("caught"),
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_mscall_throw_catch_decl_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := P.MS(false);
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_mscall_throw_catch_decl_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := P.MS(false);
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_mscall_throw_catch_asg_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := P.MS(false);
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_mscall_throw_catch_asg_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := P.MS(false);
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_mscall_throw_catch_test_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := P.MS(false);
        P.Boom(true);
        s := "y";
      catch
        InvalidOperationException => P.Log("caught"),
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), kNullableArgDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_mscall_throw_catch_test_len_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := P.MS(false);
        P.Boom(true);
        s := "y";
      catch
        InvalidOperationException => P.Log("caught"),
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nowrite_catch_decl_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(true);
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nowrite_catch_decl_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(true);
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nowrite_catch_asg_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      P.Boom(true);
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nowrite_catch_asg_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      P.Boom(true);
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nowrite_catch_test_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        P.Boom(true);
      catch
        InvalidOperationException => P.Log("caught"),
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nowrite_catch_test_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        P.Boom(true);
      catch
        InvalidOperationException => P.Log("caught"),
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nonnull_write_catch_decl_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := "y";
      P.Boom(true);
      s := "z";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nonnull_write_catch_decl_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := "y";
      P.Boom(true);
      s := "z";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nonnull_write_catch_asg_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := "y";
      P.Boom(true);
      s := "z";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nonnull_write_catch_asg_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := "y";
      P.Boom(true);
      s := "z";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nonnull_write_catch_test_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := "y";
        P.Boom(true);
        s := "z";
      catch
        InvalidOperationException => P.Log("caught"),
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_nonnull_write_catch_test_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := "y";
        P.Boom(true);
        s := "z";
      catch
        InvalidOperationException => P.Log("caught"),
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), 0, "caught\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_nonnull_write_decl_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(false);
    finally
      s := "z";
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_nonnull_write_decl_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(false);
    finally
      s := "z";
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_nonnull_write_asg_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      P.Boom(false);
    finally
      s := "z";
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_nonnull_write_asg_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      P.Boom(false);
    finally
      s := "z";
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_nonnull_write_test_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        P.Boom(false);
      finally
        s := "z";
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_nonnull_write_test_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        P.Boom(false);
      finally
        s := "z";
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_finally_only_decl_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    finally
      P.Log("fin");
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "fin\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_finally_only_decl_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    finally
      P.Log("fin");
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "fin\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_finally_only_asg_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    finally
      P.Log("fin");
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "fin\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_finally_only_asg_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    finally
      P.Log("fin");
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "fin\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_finally_only_test_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(false);
        s := "y";
      finally
        P.Log("fin");
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), 0, "fin\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_finally_only_test_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(false);
        s := "y";
      finally
        P.Log("fin");
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), 0, "fin\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_returns_decl_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => do return 3; end,
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 3, "");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_returns_decl_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => do return 3; end,
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 3, "");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_returns_asg_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => do return 3; end,
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 3, "");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_returns_asg_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => do return 3; end,
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 3, "");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_returns_test_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(true);
        s := "y";
      catch
        InvalidOperationException => do return 3; end,
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), 3, "");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_returns_test_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(true);
        s := "y";
      catch
        InvalidOperationException => do return 3; end,
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), 3, "");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_rethrows_decl_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    catch
      InvalidOperationException => throw,
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_rethrows_decl_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    catch
      InvalidOperationException => throw,
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_rethrows_asg_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    catch
      InvalidOperationException => throw,
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_rethrows_asg_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(false);
      s := "y";
    catch
      InvalidOperationException => throw,
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_rethrows_test_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(false);
        s := "y";
      catch
        InvalidOperationException => throw,
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_null_reset_catch_rethrows_test_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(false);
        s := "y";
      catch
        InvalidOperationException => throw,
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_then_reset_decl_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(false);
    finally
      s := null;
      s := "w";
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_then_reset_decl_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(false);
    finally
      s := null;
      s := "w";
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_then_reset_asg_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      P.Boom(false);
    finally
      s := null;
      s := "w";
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_then_reset_asg_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      P.Boom(false);
    finally
      s := null;
      s := "w";
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_then_reset_test_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        P.Boom(false);
      finally
        s := null;
        s := "w";
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_then_reset_test_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        P.Boom(false);
      finally
        s := null;
        s := "w";
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           catch_resets_decl_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => do s := "c"; end,
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           catch_resets_decl_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => do s := "c"; end,
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           catch_resets_asg_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => do s := "c"; end,
    end try;
    Console.WriteLine(S.F(s));
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           catch_resets_asg_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    s := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => do s := "c"; end,
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           catch_resets_test_arg_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(true);
        s := "y";
      catch
        InvalidOperationException => do s := "c"; end,
      end try;
      Console.WriteLine(S.F(s));
    end if;
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           catch_resets_test_len_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.MS(true);
    if s <> null then
      try
        s := null;
        P.Boom(true);
        s := "y";
      catch
        InvalidOperationException => do s := "c"; end,
      end try;
      Console.WriteLine(s.Length);
    end if;
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           ref_arg_try_catch_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Clear(ref s);
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           nested_try_outer_catch_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      try
        s := null;
        P.Boom(true);
        s := "y";
      catch
        ArgumentException => P.Log("inner"),
      end try;
    catch
      InvalidOperationException => P.Log("outer"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           catch_reads_after_interior_null_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      P.Boom(true);
      s := "y";
    catch
      InvalidOperationException => Console.WriteLine(s.Length),
    end try;
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           while_in_try_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    var i := 0;
    try
      while i < 1 do
        s := null;
        P.Boom(true);
        s := "y";
        i := i + 1;
      end while;
    catch
      InvalidOperationException => P.Log("caught"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_ref_arg_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(false);
    finally
      P.Clear(ref s);
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           finally_null_in_branch_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(false);
    finally
      if P.Boom(false) = 1 then s := null; end if;
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           catch_arm_null_then_rethrow_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(false);
    catch
      InvalidOperationException => do s := null; throw; end,
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_establishes_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    try
      s := "x";
      P.Boom(false);
    catch
      InvalidOperationException => do s := "c"; end,
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           try_establishes_finally_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    try
      s := "x";
      P.Boom(false);
    finally
      P.Log("fin");
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "fin\n1\n");
}

BEZEL_TEST(sema_try_interior_kill_6673,
           break_through_finally_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    while true do
      try
        break;
      finally
        s := null;
      end try;
    end while;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_interior_kill_6673,
           return_in_body_catch_establishes_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    try
      if P.Boom(true) = 1 then return 5; end if;
      return 6;
    catch
      InvalidOperationException => do s := "c"; end,
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "1\n");
}
