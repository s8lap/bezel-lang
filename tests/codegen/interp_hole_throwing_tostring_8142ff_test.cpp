// Regression tests for the #8142 fix-forward (§8.5 / §18.5 / §31.2, v0.355):
// a VIRTUALLY dispatched `ToString` that THROWS must unwind to the enclosing
// `catch`, whichever spelling reached the dispatch.
//
// §8.5 (v0.355), first sentence: "`ToString` is invoked implicitly inside
// string interpolation (§18.5) via virtual dispatch". The dispatch is a call,
// and §31.2's `try`/`catch` handles an exception raised by any call in the
// protected block.
//
// THE DEFECT. #8142 (833c2c72) routed a class-typed interpolation hole through
// `emitValueToString` -> `lowerObjectToString` (the receiver's vtable, slot 3).
// That dispatch was a bare call with NO pending check: a throwing override
// leaves its exception in the in-flight slot and returns null, and the caller
// consumed the null as a string. MEASURED on r30-c1 (833c2c72 picked):
//   * `$"{t}"` in a try body SEGVs, reading 0x8 in `main` (the issue's repro);
//   * `t.ToString()` (the member call, already on this road before #8142)
//     prints an EMPTY line and `caught=0` — the throw was silently lost;
//   * the `Object`-typed and interface-typed holes SEGV the same way.
// With #8142's two source edits reverted, the class hole was `caught=1` again
// (the static-chain `emitMethodCall` path checks), but the member call, the
// Object hole and the interface hole were broken there too.
//
// THE FIX: `lowerObjectToString` runs `emitPendingCheck()` after the dispatch,
// as `emitMethodCall` does after a resolved call; and every Setup-time
// object-protocol thunk (`ThunkScope`) now isolates the per-function unwind
// state, so the same check inside a record-rendering thunk unwinds to the
// THUNK's own propagate block instead of into the body being emitted.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked — the checked oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// ── the issue's repro, verbatim ─────────────────────────────────────────────
//
// MEASURED: r30-c1 SEGVs (read of 0x8 in `main`, r.ran false); fixed prints
// `caught=1`.
BEZEL_TEST(interp_hole_throwing_tostring_8142ff,
           repro_throwing_hole_unwinds_to_the_enclosing_catch) {
    auto r = runAotChecked(R"(
module M;
public class T
  public constructor() end constructor;
  public override method ToString(): string
    throw new InvalidOperationException("render");
  end ToString;
end T;
public class Program
  public static method Main(): int
    var caught := 0;
    var t := new T();
    try
      Console.WriteLine($"{t}");
    catch
      InvalidOperationException => do caught := caught + 1; end,
    end try;
    Console.WriteLine("caught=" + caught.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "caught=1\n");
}

// ── the same repro 50x under the checked oracle: no leak, no double free ────
//
// A one-shot leak repro can hide under the sanitizer's noise; the loop makes
// a per-iteration leak of the in-flight exception or of the receiver visible.
BEZEL_TEST(interp_hole_throwing_tostring_8142ff,
           repro_50x_loop_no_leak_and_every_throw_caught) {
    auto r = runAotChecked(R"(
module M;
public class T
  public constructor() end constructor;
  public override method ToString(): string
    throw new InvalidOperationException("render");
  end ToString;
end T;
public class Program
  public static method Main(): int
    var caught := 0;
    var i := 0;
    while i < 50 do
      var t := new T();
      try
        Console.WriteLine($"{t}");
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("caught=" + caught.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "caught=50\n");
}

// ── a derived class behind an ABSTRACT base ─────────────────────────────────
//
// The hole's static type is the abstract base, which declares no `ToString`;
// only the RUNTIME class's override throws. A non-throwing sibling (`Ok`)
// renders through the same dispatch, so the row also proves the check does
// not fire on a normal return, and the base-typed MEMBER call is checked too.
// The THROWING holes carry no literal text: a throw out of a hole that has an
// accumulated prefix (`$"[{a}]"`) leaks that accumulator on the unwind edge,
// which is #8172's defect (pinned by `interp_hole_residual_8171`), not this one.
BEZEL_TEST(interp_hole_throwing_tostring_8142ff,
           abstract_base_hole_with_throwing_derived_override) {
    auto r = runAotChecked(R"(
module M;
public abstract class Shape
  public abstract method Area(): int;
end Shape;
public class Bad: Shape
  public constructor() end constructor;
  public override method Area(): int => 1;
  public override method ToString(): string
    throw new InvalidOperationException("bad");
  end ToString;
end Bad;
public class Ok: Shape
  public constructor() end constructor;
  public override method Area(): int => 2;
  public override method ToString(): string => "ok";
end Ok;
public class Program
  public static method Render(s: Shape): string => $"{s}";
  public static method Main(): int
    var caught := 0;
    var i := 0;
    while i < 20 do
      var a: Shape := new Bad();
      var b: Shape := new Ok();
      try
        Console.WriteLine($"[{b}]");
        Console.WriteLine($"{a}");
        Console.WriteLine("unreached");
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      try
        Console.WriteLine(Render(a));
      catch
        InvalidOperationException => do caught := caught + 100; end,
      end try;
      try
        var s := a.ToString();
        Console.WriteLine(s);
      catch
        InvalidOperationException => do caught := caught + 10000; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("caught=" + caught.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expected;
    for (int k = 0; k < 20; ++k) expected += "[ok]\n";
    expected += "caught=202020\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// ── the other spellings on the SAME dispatch ────────────────────────────────
//
// The member call `t.ToString()`, the `Object`-typed hole, the interface-typed
// hole and the console argument all reach `lowerObjectToString`. MEASURED on
// r30-c1: the member call printed an empty line and lost the throw; the
// Object and interface holes SEGV'd. Each sits in its own try so a lost throw
// shows as a missing digit in `caught`.
BEZEL_TEST(interp_hole_throwing_tostring_8142ff,
           member_call_object_interface_and_console_spellings_unwind) {
    auto r = runAotChecked(R"(
module M;
public interface IR
  method Tag(): int;
end IR;
public class T: IR
  public constructor() end constructor;
  public method Tag(): int => 1;
  public override method ToString(): string
    throw new InvalidOperationException("render");
  end ToString;
end T;
public class Program
  public static method Main(): int
    var caught := 0;
    var i := 0;
    while i < 10 do
      var t := new T();
      try
        var s := t.ToString();
        Console.WriteLine(s);
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      try
        var o: Object := t;
        Console.WriteLine($"{o}");
      catch
        InvalidOperationException => do caught := caught + 10; end,
      end try;
      try
        var ir: IR := t;
        Console.WriteLine($"{ir}");
      catch
        InvalidOperationException => do caught := caught + 100; end,
      end try;
      try
        Console.WriteLine(t);
      catch
        InvalidOperationException => do caught := caught + 1000; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("caught=" + caught.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "caught=11110\n");
}

// ── records with class-typed components: the THUNK side ──────────────────
//
// §8.5's component rule renders a class-typed component by virtual dispatch,
// so every record's Setup-time ToString thunk now holds the dispatch and its
// pending check. Without the ThunkScope isolation, the FIRST such thunk
// created its propagate block and left the emitter's `propagateBB_` pointing
// at it, and the SECOND thunk branched there: "Referring to a basic block in
// another function" (IR verification failed) on a program that throws
// nothing. MEASURED with only the `lowerObjectToString` hunk applied: this
// program does not compile. The row pins that it compiles and renders both
// components (a no-override class's qualified name, an override's text).
BEZEL_TEST(interp_hole_throwing_tostring_8142ff,
           two_records_with_class_components_compile_and_render) {
    auto r = runAotChecked(R"(module M;
public class N
  public constructor() end constructor;
end N;
public class P
  public constructor() end constructor;
  public override method ToString(): string => "p!";
end P;
public record Q(A: N);
public record R(B: P);
public class Program
  public static method Main(): int
    var n := new N();
    var p := new P();
    var i := 0;
    while i < 3 do
      try
        var q := new Q(n);
        var r := new R(p);
        Console.WriteLine($"{q}|{r}|" + q.ToString() + "|" + r.ToString());
      catch
        InvalidOperationException => do Console.WriteLine("caught"); end,
      end try;
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string one = "Q(A: M.N)|R(B: p!)|Q(A: M.N)|R(B: p!)\n";
    BEZEL_EXPECT_EQ(r.stdoutText, one + one + one);
}
