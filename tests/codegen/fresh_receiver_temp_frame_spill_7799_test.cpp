// The K8 receiver-temp slot of a fresh owned receiver survives a suspension in
// a state-machine body (fix #7799).
//
// §28.3 *Unbound owned temporaries* and K8.1 (v0.350): an unbound owned
// temporary used only by borrowing operations "is disposed at the end of the
// full expression in which it is produced, after every use of it within that
// expression has completed", "on the normal edge as on the exception edge".
// §30.13 (v0.350): an async continuation "runs without interruption ... until
// it suspends at an `await`"; the code after the suspension is a LATER
// continuation (a fresh `$Resume` activation, possibly on another worker
// thread), so nothing on the first activation's stack survives into it.
//
// THE DEFECT. `Make.MkH(i).Name()` queues its receiver for the full
// expression's end (#4552, registerFreshNewReceiverTempK8) in an entry-block
// pointer alloca. In a state-machine body entryAllocaNullPtr skips the entry
// null store (it expects the frame calloc to zero a frame-spilled slot), but
// the slot was never registered for the frame spill. When an `await` in the
// same full expression suspends, the flush in `$Resume` reads that alloca from
// a different stack: a wild dispose (SEGV) or a silent leak. An exception
// after a suspension reads it on the unwind edge the same way.
//
// THE ORACLE. Every row is `runAotChecked` (generated-code ASan + LSan), loops
// 100 times, and reads the destructor count off stdout (exit codes truncate at
// 8 bits). The receiver owns a `Res` whose destructor counts, so the count is
// the number of receivers disposed.
//
// MEASURED on the base (dfb9512f), bezelc --sanitize=address, loop x100:
// await_argument -> SEGV in Main$Resume, no output; receiver_before_await ->
// prints "4790 0" plus a LeakSanitizer report (nothing disposed);
// await_in_async_method -> SEGV, no output; throw_after_await ->
// stack-use-after-return in Body$Resume. The iterator row passes on the base
// (MoveNext is re-entered, and the flush left the slot null) and pins that the
// frame spill changes nothing there.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

const char* kPrelude = R"(
module M;
public class Res
  public static Count: int := 0;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
public class H
  public owned C: Res := new Res();
  public S: string;
  public constructor(n: int)
    S := "heap-" + n.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
  end constructor;
  public method Name(): string => S;
end H;
public class Make
  public static method MkH(n: int): owned H return new H(n); end MkH;
end Make;
)";

// "heap-" (5) + digits + "-" (1) + 39 x: 46 characters for a one-digit n, 47
// for two digits; i in 0..<100 sums to 4690.

void expectRun(const std::string& program, const std::string& expected) {
    bezel::test::JitResult r = runAotChecked(std::string(kPrelude) + program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// The issue's repro: the receiver's string result is the argument of the
// awaited call, so the flush runs after the suspension.
BEZEL_TEST(fresh_receiver_temp_frame_spill,
           await_argument_receiver_disposed_after_resume_spec_28_3_7799) {
    expectRun(R"(
public class Program
  public static async method Af(s: string): Task<int> await Task.Delay(1); return s.Length; end Af;
  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0..<100 do
      t := t + await Af(Make.MkH(i).Name());
    end;
    Console.WriteLine($"{t} {Res.Count}");
    return 0;
  end Main;
end Program;
)", "4690 100\n");
}

// The receiver is produced and fully used BEFORE the suspension, in the same
// full expression; its disposal is still at the expression's end (K8.1).
BEZEL_TEST(fresh_receiver_temp_frame_spill,
           receiver_before_await_same_expression_spec_28_3_7799) {
    expectRun(R"(
public class Program
  public static async method Dly(i: int): Task<int> await Task.Delay(1); return 1; end Dly;
  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0..<100 do
      t := t + Make.MkH(i).Name().Length + await Dly(i);
    end;
    Console.WriteLine($"{t} {Res.Count}");
    return 0;
  end Main;
end Program;
)", "4790 100\n");
}

// The same shape in a `return` of a non-Main async method.
BEZEL_TEST(fresh_receiver_temp_frame_spill,
           await_in_async_method_return_spec_28_3_7799) {
    expectRun(R"(
public class Program
  public static async method Dly(i: int): Task<int> await Task.Delay(1); return 1; end Dly;
  public static async method Body(i: int): Task<int> return Make.MkH(i).Name().Length + await Dly(i); end Body;
  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0..<100 do
      t := t + await Body(i);
    end;
    Console.WriteLine($"{t} {Res.Count}");
    return 0;
  end Main;
end Program;
)", "4790 100\n");
}

// K8.1's exception edge: the receiver was disposed at its own statement's end;
// a throw after a LATER suspension must not dispose it again from the
// (stale) slot.
BEZEL_TEST(fresh_receiver_temp_frame_spill,
           throw_after_await_unwind_reads_frame_slot_spec_28_3_7799) {
    expectRun(R"(
public class Program
  public static Caught: int := 0;
  public static method Bump() Program.Caught := Program.Caught + 1; end Bump;
  public static async method Dly(i: int): Task<int> await Task.Delay(1); return 1; end Dly;
  public static async method Body(i: int): Task<int>
    var t := Make.MkH(i).Name().Length;
    t := t + await Dly(i);
    if i >= 0 then throw new InvalidOperationException("boom"); end;
    return t;
  end Body;
  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0..<100 do
      try
        t := t + await Body(i);
      catch
        InvalidOperationException => Program.Bump(),
      end try;
    end;
    Console.WriteLine($"{t} {Res.Count} {Program.Caught}");
    return 0;
  end Main;
end Program;
)", "0 100 100\n");
}

// Control (passes on the base): an iterator body, where a `yield` separates
// the receiver's statement from a later throw. Each call yields its string's
// length twice (k = 0, 1) before the throw: 2 x 4690 = 9380, two receivers per
// call.
BEZEL_TEST(fresh_receiver_temp_frame_spill,
           iterator_throw_after_yield_control_spec_28_3_7799) {
    expectRun(R"(
public class Program
  public static Caught: int := 0;
  public static method Bump() Program.Caught := Program.Caught + 1; end Bump;
  public static method Gen(n: int): IEnumerable<int>
    for var k in 0..<3 do
      var t := Make.MkH(n).Name().Length;
      yield return t;
      if k = 1 then throw new InvalidOperationException("boom"); end;
    end;
  end Gen;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      try
        for var x in Gen(i) do t := t + x; end;
      catch
        InvalidOperationException => Program.Bump(),
      end try;
    end;
    Console.WriteLine($"{t} {Res.Count} {Program.Caught}");
    return 0;
  end Main;
end Program;
)", "9380 200 100\n");
}
