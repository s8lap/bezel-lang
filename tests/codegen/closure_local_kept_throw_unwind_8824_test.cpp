// Codegen — issue #8824: a closure local stored into a collection and then
// reassigned (`fs.Add(b); Boom(k); b := g;`) had its stored value released on
// the UNWIND path when a call between the store and the reassignment threw
// out of the method, so the caller's collection held a freed closure.
//
// THE RULE. §28.9 (v0.359, "Runtime-managed types"): for a function-typed
// value, "the type's lifetime is managed by the runtime without user
// involvement", so a function value a collection still holds stays callable
// whichever way the frame that stored it was left. §27.5 (v0.359): a
// closure's captures live as long as the closure.
//
// ROOT CAUSE. #8753's reaching-definition analysis (the donor pass at the end
// of `collectClosureReleaseEligible`) modelled the method's exits as its
// normal end, `return` and a `throw` STATEMENT. An exception raised by a CALL
// that leaves the method runs `b`'s scope-end release on the unwind path
// too, and at that point `b` still held its initial value, which `fs` keeps
// without a reference (#5866).
//
// THE FIX. Every statement boundary may be such an exit, so its state is
// joined into the analysis' unwind state. If a kept value of the slot's own
// can reach it, `b`'s scope-end release is marked
// `closureReleaseSkipOnUnwind` and fires on the normal exits only
// (closureReassignedKeptAtUnwind_). What remains is the #5866 leak of the
// stored closure, so these rows use `runAotCheckedLenient` (AddressSanitizer
// on, LeakSanitizer off) with the full stdout compared.
//
// Every program loops 200 times; the odd iterations throw out of `Fill`.
//
// MEASURED (`bezelc --sanitize=address --link=system`, detect_leaks=1, on
// r30-c1 @ 432dfa61c): every row below prints the k=0 line and then reports
// heap-use-after-free at k=1; with the fix, 201 correct lines and the #5866
// leak.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotCheckedLenient;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired (AddressSanitizer on, LeakSanitizer off); a marker otherwise.
std::string runNoMemoryErrorOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// Iteration i calls the stored closure `x - y + i` twice:
// (1 - 2 + i) + (4 - 1 + i).
std::string expected200() {
    std::string e;
    for (int i = 0; i < 200; ++i) e += std::to_string(2 + 2 * i) + "\n";
    e += "DONE\n";
    return e;
}

// `fillBody` follows the declarations of `g` (returns 0) and `b` (returns
// `x - y + k`); it must leave `b`'s initial closure in `fs[0]`. `Boom(k)`
// throws on odd `k`; `Main` catches what leaves `Fill`.
std::string fillProgram(const std::string& fillBody,
                        const std::string& members = "") {
    return R"BZL(
module M;
type Fn := (int, int) -> int;
public class C
  public static method Boom(k: int)
    if k mod 2 = 1 then
      throw new InvalidOperationException("x");
    end;
  end Boom;
)BZL" + members + R"BZL(
  public static method Fill(fs: Array<Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
)BZL" + fillBody + R"BZL(
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      try
        Fill(fs, i);
      catch
        var e: Exception => do var z := 0; end
      end try;
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL";
}

// narrow #8824: a catch guard's store (#8769's catch-entry state) must also
// reach the unwind state. `Fill`'s exception leaves the method when the guard
// is false.
std::string guardExpected200() {
    std::string e;
    for (int i = 1; i < 200; i += 2) e += std::to_string(2 + 2 * i) + "\n";
    e += "100\n";
    return e;
}

}  // namespace

// #8824's repro (c07), as filed.
BEZEL_TEST(closure_local_kept_throw_unwind_8824,
           issue_repro_add_throwing_call_then_reassign_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    Boom(k);\n"
        "    b := g;\n")),
        expected200());
}

// The exception is caught in Fill's own frame, and the catch arm's own call
// raises a second one, which leaves the method (§31.1).
BEZEL_TEST(closure_local_kept_throw_unwind_8824,
           add_then_throwing_call_in_own_catch_arm_spec_31_1) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    try\n"
        "      Boom(k);\n"
        "    catch\n"
        "      var e: InvalidOperationException => do Boom(1); end\n"
        "    end try;\n"
        "    b := g;\n")),
        expected200());
}

// Nested `try` statements whose catch arms do not match, so the exception
// leaves the method (§31.2).
BEZEL_TEST(closure_local_kept_throw_unwind_8824,
           add_then_throw_through_nested_try_spec_31_2) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    try\n"
        "      try\n"
        "        fs.Add(b);\n"
        "        Boom(k);\n"
        "        b := g;\n"
        "      catch\n"
        "        var e: ArgumentException => do var z := 1; end\n"
        "      end try;\n"
        "    catch\n"
        "      var e2: FormatException => do b := g; end\n"
        "    end try;\n"
        "    b := g;\n")),
        expected200());
}

// The throwing call is in a `while` condition.
BEZEL_TEST(closure_local_kept_throw_unwind_8824,
           add_then_throwing_call_in_while_condition_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    var j := 0;\n"
        "    while j < 1 and C.Check(k) do\n"
        "      j := j + 1;\n"
        "    end while;\n"
        "    b := g;\n",
        "  public static method Check(k: int): bool\n"
        "    Boom(k);\n"
        "    return true;\n"
        "  end Check;\n")),
        expected200());
}

// The exception is raised inside a block lambda called through a local
// (§27.5).
BEZEL_TEST(closure_local_kept_throw_unwind_8824,
           add_then_throwing_lambda_call_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    var t: (int) -> int := lambda(n: int) => do\n"
        "      if n mod 2 = 1 then\n"
        "        throw new InvalidOperationException(\"t\");\n"
        "      end;\n"
        "      return n;\n"
        "    end;\n"
        "    fs.Add(b);\n"
        "    var r := t(k);\n"
        "    b := g;\n")),
        expected200());
}

// The throwing call is the reassignment's own right-hand side: `b` still
// holds the kept value when the exception leaves.
BEZEL_TEST(closure_local_kept_throw_unwind_8824,
           add_then_reassign_from_throwing_call_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    b := C.Pick(k);\n",
        "  public static method Pick(k: int): Fn\n"
        "    Boom(k);\n"
        "    return lambda(x: int, y: int) => 0;\n"
        "  end Pick;\n")),
        expected200());
}

// Inside a loop: the store on the first iteration, a throwing call before
// each reassignment.
BEZEL_TEST(closure_local_kept_throw_unwind_8824,
           add_in_loop_then_throwing_call_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    for var j in 0..<2 do\n"
        "      if j = 0 then\n"
        "        fs.Add(b);\n"
        "      end;\n"
        "      Boom(k + j);\n"
        "      b := g;\n"
        "    end for;\n")),
        expected200());
}

// narrow #8824: definition before the try; the false guard's `d.TryAdd(0, b)` kept `b`, and the
// exception leaves Fill (§28.9, §31.1, v0.359).
BEZEL_TEST(closure_local_kept_throw_unwind_8824, guard_false_keeps_single_stmt_arm_def_before_try_spec_28_9_31_1) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(module M;
type Fn := (int, int) -> int;
public class C
  public static method Boom(k: int)
    if k mod 2 = 1 then
      throw new InvalidOperationException("x");
    end;
  end Boom;
  public static method Fill(d: Dictionary<int, Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
    try
      Boom(k);
    catch
      var e: InvalidOperationException when d.TryAdd(0, b) and k < 0 => b := g;
    end try;
    b := g;
  end Fill;
  public static method Main(): int
    var n := 0;
    for var i in 0..<200 do
      var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();
      try
        Fill(d, i);
      catch
        var e: InvalidOperationException => do n := n + 1; end
      end try;
      if d.ContainsKey(0) then
        Console.WriteLine(d[0](1, 2) + d[0](4, 1));
      end;
    end for;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
        guardExpected200());
}

// narrow #8824: definition inside the try body; the false guard's `d.TryAdd(0, b)` kept `b`, and the
// exception leaves Fill (§28.9, §31.1, v0.359).
BEZEL_TEST(closure_local_kept_throw_unwind_8824, guard_false_keeps_def_inside_try_body_spec_28_9_31_1) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(module M;
type Fn := (int, int) -> int;
public class C
  public static method Boom(k: int)
    if k mod 2 = 1 then
      throw new InvalidOperationException("x");
    end;
  end Boom;
  public static method Fill(d: Dictionary<int, Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := g;
    try
      b := lambda(x: int, y: int) => x - y + k;
      Boom(k);
    catch
      var e: InvalidOperationException when d.TryAdd(0, b) and k < 0 => b := g;
    end try;
    b := g;
  end Fill;
  public static method Main(): int
    var n := 0;
    for var i in 0..<200 do
      var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();
      try
        Fill(d, i);
      catch
        var e: InvalidOperationException => do n := n + 1; end
      end try;
      if d.ContainsKey(0) then
        Console.WriteLine(d[0](1, 2) + d[0](4, 1));
      end;
    end for;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
)BZL"),
        guardExpected200());
}
