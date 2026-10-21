// Spec-derived regression tests for issue #6730 (the v0.340 tracker for
// proposal 0198) and issue #6682: §7.4 *Narrowing across `try`*.
//
// THE RULE. Spec §7.4 (v0.340), *Narrowing across `try`*: "every statement of
// the block is a possible throw point for every `catch` arm and for the
// `finally`. A `catch` arm and the `finally` block therefore begin with the
// facts that held at entry to the protected block, less every fact the block
// invalidates at any point (by the forms above), and no fact the block
// establishes reaches them; a later arm's entry further loses what an earlier
// arm's guard invalidates (§31.2.1: a guard runs whether or not its arm is then
// selected), and the `finally`'s entry loses what any arm's guard or body
// invalidates. The point after the statement is reached only through the
// block's normal completion or an arm's exit, each through the `finally`: it
// holds the facts common to those exits — the every-path rule — and the
// `finally`'s own statements then establish and invalidate as any straight-line
// code does. A `break` or `continue` leaving the block or an arm carries its
// own path's facts to its target, as elsewhere, the `finally`'s own statements
// applying to them on the way". Its examples: "`try s := null; s := "y"; catch
// IOException => … end; s.Length` and `s := null; try s := "x"; Log("a");
// finally s.Length end` are compile errors at the read, while `try s :=
// Fetch(); finally Log(); end; s.Length`, with no arm, is admitted". §7.4's
// establishing forms include "a successful `is`/typed-pattern test" and "a
// `when` guard (§19.4, narrowing that arm's body)", so the paragraph governs
// the §12.7 type facts as well as the null facts.
//
// WHAT WAS BROKEN. MEASURED on 97f4e965 with `bezelc --sanitize=address`
// (probes in /var/tmp/bezel-scratch/r34-6730/probes):
//   - #6682: a `break`/`continue` edge was snapshotted at the jump, before the
//     `finally` was walked. A `finally` that stores `null` (or reassigns a
//     type-narrowed local) never reached the edge, so when the loop body
//     re-established the fact after the `try`, the after-loop join and the
//     back-edge check trusted the stale edge. The `cond_break_…`,
//     `cond_continue_…`, `arm_break_…`, `for_break_…`, `nested_try_…` and
//     `field_cond_break_…` rows compiled and segfaulted. The `type_continue_…`
//     row compiled and called `Bark` on an `Animal`.
//   - The §12.7 type facts were threaded straight through the `try`. A catch
//     arm and the `finally` were walked in the block's exit state, and the
//     `if` join keeps an entry fact whose invalidating branch leaves. So
//     `if c then a := new Animal(); Boom(); return 0; end` left `a` a `Dog` in
//     the arm, in the `finally` and after the `try`. The `type_exiting_branch_…`
//     rows compiled and called `Bark` on an `Animal`. A pending #6083 record
//     made in the block reached the arm too. The `type_record_…` rows compiled
//     and admitted `Object` to `Dog` on an `Animal`.
//   - Over-rejections. A later arm was entered in the earlier arm's EXIT state,
//     not its guard's, so a store in an earlier arm's body rejected a later
//     arm's read (`…earlier_arm_body_kill…`, all three fact sets). A type fact
//     an arm invalidated before leaving (`return`) was lost after the `try`
//     (`type_leaving_arm_kill…`).
// The other rows are pins. They passed on 97f4e965 (the #6673 fix) and are
// the tracker's coverage gaps: c03, c08, b18 and d05 (the #6673 reviewer's
// probes), the plain `break` through a reassigning `finally`, the no-arm read
// and a guarded arm narrowing its own body.

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
class Animal
  public method Name(): string => "a";
end Animal;
class Dog: Animal
  public method Bark(): string => "woof";
end Dog;
class H
  public readonly _f: string?;
  public constructor(f: string?)
    _f := f;
  end constructor;
end H;
public class P
  public static method Boom(f: bool): int
    if f then throw new InvalidOperationException("x"); end;
    return 1;
  end Boom;
  public static method ClearB(ref v: string?): bool
    v := null;
    return false;
  end ClearB;
  public static method Log(m: string)
    Console.WriteLine(m);
  end Log;
  public static method Fetch(): string => "abc";
  public static method Maybe(f: bool): string?
    if f then return "mm"; end;
    return null;
  end Maybe;
  public static method Mk(d: bool): owned Animal
    if d then return new Dog(); end;
    return new Animal();
  end Mk;
  public static method No(): bool => false;
)bzl";

constexpr const char* kTail = "end P;\n";

JitResult run(const char* body) {
    return runAot(std::string(kPrelude) + body + kTail);
}

const char* const kNullableMemberDiag =
    "member access `.Length` on a nullable value requires narrowing";
const char* const kNullableFieldDiag =
    "member access `.Length` on a nullable field or property requires "
    "narrowing";
const char* const kNoBarkDiag = "no member `Bark` on type `Animal`";
const char* const kLoopNullDiag =
    "`s` is nullable here: the knowledge that it is non-null was established "
    "before the loop";
const char* const kLoopTypeDiag =
    "`a` does not have its narrowed type here: the type narrowing was "
    "established before the loop";
const char* const kDowncastDiag =
    "`Object` to `Dog` is a downcast, which is never implicit";

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

BEZEL_TEST(sema_try_narrowing_6730,
           c03_arm_entry_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      s := "y";
    catch
      InvalidOperationException => Console.WriteLine(s.Length),
    end try;
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           c03_after_try_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      s := null;
      s := "y";
    catch
      InvalidOperationException => P.Log("c"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           c08_finally_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    try
      s := "x";
      P.Log("a");
    finally
      Console.WriteLine(s.Length);
    end try;
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           b18_arm_store_then_rethrow_outer_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      try
        s := null;
        P.Boom(true);
      catch
        InvalidOperationException => do s := "z"; throw; end,
      end try;
    catch
      Exception => P.Log("outer"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           d05_three_deep_rethrow_chain_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      try
        try
          P.Boom(true);
        catch
          InvalidOperationException => do s := null; throw; end,
        end try;
      catch
        InvalidOperationException => do s := "w"; throw; end,
      end try;
    catch
      Exception => P.Log("outer"),
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           break_through_reassigning_finally_read_after_loop_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    while true do
      try
        P.Log("b");
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

BEZEL_TEST(sema_try_narrowing_6730,
           earlier_arm_guard_kill_later_arm_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(true);
    catch
      InvalidOperationException when P.ClearB(ref s) => P.Log("a"),
      Exception => Console.WriteLine(s.Length),
    end try;
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           arm_kill_finally_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(true);
    catch
      InvalidOperationException => do s := null; end,
    finally
      Console.WriteLine(s.Length);
    end try;
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           cond_break_finally_kill_reset_after_loop_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    while true do
      try
        if P.Boom(false) = 1 then break; end;
      finally
        s := null;
      end try;
      s := "again";
    end while;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           cond_continue_finally_kill_reset_loop_head_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    var i := 0;
    while i < 3 do
      i := i + 1;
      Console.WriteLine(s.Length);
      try
        if i = 1 then continue; end;
      finally
        s := null;
      end try;
      s := "again";
    end while;
    return 0;
  end Main;
)bzl"), kLoopNullDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           arm_break_finally_kill_reset_after_loop_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    while true do
      try
        P.Boom(true);
      catch
        InvalidOperationException => do break; end,
      finally
        s := null;
      end try;
      s := "again";
    end while;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           for_break_finally_kill_reset_after_loop_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    for var i in 0 ..< 3 do
      try
        if i = 0 then break; end;
      finally
        s := null;
      end try;
      s := "again";
    end for;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           nested_try_break_outer_finally_kill_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    while true do
      try
        try
          if P.Boom(false) = 1 then break; end;
        finally
          P.Log("in");
        end try;
      finally
        s := null;
      end try;
      s := "again";
    end while;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), kNullableMemberDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           field_cond_break_finally_kill_reset_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var h := new H("abc");
    if h._f <> null then
      while true do
        try
          if P.Boom(false) = 1 then break; end;
        finally
          h := new H(null);
        end try;
        h := new H("again");
        if h._f = null then return 0; end;
      end while;
      Console.WriteLine(h._f.Length);
    end if;
    return 0;
  end Main;
)bzl"), kNullableFieldDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_continue_finally_kill_loop_head_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var a: Animal := P.Mk(true);
    if a is Dog then
      var i := 0;
      while i < 3 do
        i := i + 1;
        Console.WriteLine(a.Bark());
        try
          if i = 1 then continue; end;
        finally
          a := new Animal();
        end try;
        return 0;
      end while;
    end if;
    return 0;
  end Main;
)bzl"), kLoopTypeDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           cond_break_finally_no_kill_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    while true do
      try
        if P.Boom(false) = 1 then break; end;
      finally
        P.Log("f");
      end try;
      s := "again";
    end while;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "f\n1\n");
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_c03_arm_entry_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var a: Animal := P.Mk(true);
    if a is Dog then
      try
        a := P.Mk(false);
        if a is not Dog then return 0; end;
      catch
        InvalidOperationException => Console.WriteLine(a.Bark()),
      end try;
    end if;
    return 0;
  end Main;
)bzl"), kNoBarkDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_c08_finally_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var a: Animal := P.Mk(false);
    try
      if a is not Dog then return 0; end;
      P.Log("a");
    finally
      Console.WriteLine(a.Bark());
    end try;
    return 0;
  end Main;
)bzl"), kNoBarkDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_exiting_branch_kill_then_throw_arm_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var a: Animal := P.Mk(true);
    if a is Dog then
      try
        if P.Boom(false) = 1 then
          a := new Animal();
          P.Boom(true);
          return 0;
        end;
      catch
        InvalidOperationException => Console.WriteLine(a.Bark()),
      end try;
    end if;
    return 0;
  end Main;
)bzl"), kNoBarkDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_exiting_branch_kill_then_throw_after_try_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var a: Animal := P.Mk(true);
    if a is Dog then
      try
        if P.Boom(false) = 1 then
          a := new Animal();
          P.Boom(true);
          return 0;
        end;
      catch
        InvalidOperationException => P.Log("c"),
      end try;
      Console.WriteLine(a.Bark());
    end if;
    return 0;
  end Main;
)bzl"), kNoBarkDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_exiting_branch_kill_then_throw_finally_read_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var a: Animal := P.Mk(true);
    if a is Dog then
      try
        if P.Boom(false) = 1 then
          a := new Animal();
          P.Boom(true);
          return 0;
        end;
      finally
        Console.WriteLine(a.Bark());
      end try;
    end if;
    return 0;
  end Main;
)bzl"), kNoBarkDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_record_established_in_block_arm_downcast_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var o: Object := P.Mk(false);
    try
      if o is not Dog then return 0; end;
      P.Boom(true);
    catch
      InvalidOperationException => do var d: Dog := o; Console.WriteLine(d.Bark()); end,
    end try;
    return 0;
  end Main;
)bzl"), kDowncastDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_record_kill_reestablish_arm_downcast_rejected_spec_7_4) {
    expectRejected(run(R"bzl(
  public static method Main(): int
    var o: Object := P.Mk(true);
    if o is not Dog then return 0; end;
    try
      o := P.Mk(false);
      P.Boom(true);
      o := P.Mk(true);
      if o is not Dog then return 0; end;
    catch
      InvalidOperationException => do var d: Dog := o; Console.WriteLine(d.Bark()); end,
    end try;
    return 0;
  end Main;
)bzl"), kDowncastDiag);
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_entry_fact_survives_arm_and_finally_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var a: Animal := P.Mk(true);
    if a is Dog then
      try
        P.Boom(true);
      catch
        InvalidOperationException => Console.WriteLine(a.Bark()),
      finally
        Console.WriteLine(a.Bark());
      end try;
      Console.WriteLine(a.Bark());
    end if;
    return 0;
  end Main;
)bzl"), 0, "woof\nwoof\nwoof\n");
}

BEZEL_TEST(sema_try_narrowing_6730,
           no_arm_after_try_read_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := null;
    try
      s := P.Fetch();
    finally
      P.Log("f");
    end try;
    Console.WriteLine(s.Length);
    return 0;
  end Main;
)bzl"), 0, "f\n3\n");
}

BEZEL_TEST(sema_try_narrowing_6730,
           guarded_arm_narrows_its_body_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := P.Maybe(true);
    try
      P.Boom(true);
    catch
      InvalidOperationException when s <> null => Console.WriteLine(s.Length),
      Exception => P.Log("e"),
    end try;
    return 0;
  end Main;
)bzl"), 0, "2\n");
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_guarded_arm_narrows_its_body_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var a: Animal := P.Mk(true);
    try
      P.Boom(true);
    catch
      InvalidOperationException when a is Dog => Console.WriteLine(a.Bark()),
      Exception => P.Log("e"),
    end try;
    return 0;
  end Main;
)bzl"), 0, "woof\n");
}

BEZEL_TEST(sema_try_narrowing_6730,
           earlier_arm_body_kill_later_arm_read_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var s: string? := "x";
    try
      P.Boom(true);
    catch
      ArgumentException => do s := null; end,
      Exception => Console.WriteLine(s.Length),
    end try;
    return 0;
  end Main;
)bzl"), 0, "1\n");
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_earlier_arm_body_kill_later_arm_read_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var a: Animal := P.Mk(true);
    if a is Dog then
      try
        P.Boom(true);
      catch
        ArgumentException => do a := new Animal(); end,
        Exception => Console.WriteLine(a.Bark()),
      end try;
    end if;
    return 0;
  end Main;
)bzl"), 0, "woof\n");
}

BEZEL_TEST(sema_try_narrowing_6730,
           field_earlier_arm_body_kill_later_arm_read_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var h := new H("abc");
    if h._f <> null then
      try
        P.Boom(true);
      catch
        ArgumentException => do h := new H(null); end,
        Exception => Console.WriteLine(h._f.Length),
      end try;
    end if;
    return 0;
  end Main;
)bzl"), 0, "3\n");
}

BEZEL_TEST(sema_try_narrowing_6730,
           type_leaving_arm_kill_after_try_read_runs_spec_7_4) {
    expectRuns(run(R"bzl(
  public static method Main(): int
    var a: Animal := P.Mk(true);
    if a is Dog then
      try
        P.Boom(false);
      catch
        InvalidOperationException => do a := new Animal(); return 3; end,
      end try;
      Console.WriteLine(a.Bark());
    end if;
    return 0;
  end Main;
)bzl"), 0, "woof\n");
}
