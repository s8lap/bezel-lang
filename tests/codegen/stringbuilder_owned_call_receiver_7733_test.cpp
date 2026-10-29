// Regression tests for #7733 -- an owned call-result StringBuilder receiver
// (`Fresh(sb).Insert(..)`, `Fresh(sb).Length`) was never disposed and leaked
// the builder (63000 B over 100 iterations; positional and named alike). It is
// the sibling of #7565, which disposed a fresh `new StringBuilder(...)` chain
// root; the chain root may equally be a call to a method declared to return
// `owned StringBuilder`.
//
// Language spec §28.3 (v0.350) *Unbound owned temporaries* / K8.1, verbatim:
// "Unbound owned temporaries are disposed at their full expression's end, after
// their uses, in reverse production order -- on the normal edge as on the
// exception edge, where the pass runs during unwinding." §28.1: a method
// declared `owned T` transfers a fresh +1 to its caller. stdlib §3.1:
// `Insert` / `Append` / `Remove` / `Replace` return the builder (a borrow of
// the receiver), `Length` is an `int`, `ToString()` a fresh string.
//
// Every row runs on runAotChecked (ASan + LSan) and loops 100 times: a one-shot
// leak repro is a vacuous oracle. The SOUNDNESS row pins the gate: only a
// declared-`owned` return is a fresh producer; a method that merely returns
// its (borrowed) parameter must not be disposed at the boundary, or `sb`
// dangles (a heap-use-after-free fails the row).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

void expectClean(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// The issue probe, positional: value then index evaluated in written order (T appends B, TS appends A).
BEZEL_TEST(stringbuilder_owned_call_receiver_7733, insert_positional_loop_100_spec_28_3) {
    expectClean(runAotChecked(R"BZL(
module M;
import System;
import System.Text;
class C
  public static method T(sb: StringBuilder, s: string, v: int): int
    sb.Append(s);
    return v;
  end T;
  public static method TS(sb: StringBuilder, s: string, v: string): string
    sb.Append(s);
    return v;
  end TS;
  public static method Mk(sb: StringBuilder, s: string, t: StringBuilder): StringBuilder
    sb.Append(s);
    return t;
  end Mk;
  public static method Pick(sb: StringBuilder, s: string, t: StringBuilder?): StringBuilder?
    sb.Append(s);
    return t;
  end Pick;
  public static method Fresh(sb: StringBuilder): owned StringBuilder
    sb.Append("R");
    return new StringBuilder("abc");
  end Fresh;
  public static method Main(): int
    using var sb := new StringBuilder();
    var total := 0;
    for var i in 0..<100 do
      total += Fresh(sb).Insert(T(sb, "B", 1), TS(sb, "A", "XY")).ToString().Length;
    end for;
    Console.WriteLine(total.ToString() + "|" + sb.Length.ToString() + "|" + sb.ToString().Substring(0, 3));
    return 0;
  end Main;
end C;
)BZL"),
                "500|300|RBA\n");
}

// The issue probe, named and reordered.
BEZEL_TEST(stringbuilder_owned_call_receiver_7733, insert_named_reordered_loop_100_spec_28_3) {
    expectClean(runAotChecked(R"BZL(
module M;
import System;
import System.Text;
class C
  public static method T(sb: StringBuilder, s: string, v: int): int
    sb.Append(s);
    return v;
  end T;
  public static method TS(sb: StringBuilder, s: string, v: string): string
    sb.Append(s);
    return v;
  end TS;
  public static method Mk(sb: StringBuilder, s: string, t: StringBuilder): StringBuilder
    sb.Append(s);
    return t;
  end Mk;
  public static method Pick(sb: StringBuilder, s: string, t: StringBuilder?): StringBuilder?
    sb.Append(s);
    return t;
  end Pick;
  public static method Fresh(sb: StringBuilder): owned StringBuilder
    sb.Append("R");
    return new StringBuilder("abc");
  end Fresh;
  public static method Main(): int
    using var sb := new StringBuilder();
    var total := 0;
    for var i in 0..<100 do
      total += Fresh(sb).Insert(value := TS(sb, "A", "XY"), index := T(sb, "B", 1)).ToString().Length;
    end for;
    Console.WriteLine(total.ToString() + "|" + sb.Length.ToString() + "|" + sb.ToString().Substring(0, 3));
    return 0;
  end Main;
end C;
)BZL"),
                "500|300|RAB\n");
}

// Insert/ToString, the Length property, a discarded chain statement and a multi-link chain, all rooted at the call.
BEZEL_TEST(stringbuilder_owned_call_receiver_7733, chain_forms_loop_100_spec_28_3) {
    expectClean(runAotChecked(R"BZL(
module M; import System; import System.Text;
class C
  public static method Fresh(sb: StringBuilder): owned StringBuilder
    sb.Append("R");
    return new StringBuilder("abc");
  end Fresh;
  public static method Main(): int
    using var sb := new StringBuilder();
    var total := 0;
    for var i in 0..<100 do
      total += Fresh(sb).Insert(1, "x").ToString().Length;
      total += Fresh(sb).Length;
      Fresh(sb).Append("z");
      total += Fresh(sb).Append("q").Remove(0, 1).Replace("b", "B").ToString().Length;
    end for;
    Console.WriteLine(total.ToString() + "|" + sb.Length.ToString());
    return 0;
  end Main;
end C;
)BZL"),
                "1000|400\n");
}

// An instance method, a qualified static call and an extension, each returning `owned`; the non-`owned` `Same` returns its parameter and must NOT be disposed (soundness).
BEZEL_TEST(stringbuilder_owned_call_receiver_7733, instance_qualified_and_extension_roots_loop_100_spec_28_3) {
    expectClean(runAotChecked(R"BZL(
module M; import System; import System.Text;
class H
  public method Make(sb: StringBuilder): owned StringBuilder
    sb.Append("M");
    return new StringBuilder("abc");
  end Make;
  public method Same(sb: StringBuilder): StringBuilder
    return sb;
  end Same;
end H;
extension StringBuilder
  public method Ext(n: int): owned StringBuilder
    return new StringBuilder("ext");
  end Ext;
end;
class C
  public static method Fresh(sb: StringBuilder): owned StringBuilder
    sb.Append("R");
    return new StringBuilder("abc");
  end Fresh;
  public static method Main(): int
    using var sb := new StringBuilder();
    using var h := new H();
    var total := 0;
    for var i in 0..<100 do
      total += h.Make(sb).Insert(1, "x").ToString().Length;
      total += C.Fresh(sb).Append("q").Length;
      total += h.Same(sb).Length;
      total += h.Same(sb).Append("w").ToString().Length;
      total += sb.Ext(1).Insert(0, "e").Length;
    end for;
    Console.WriteLine(total.ToString() + "|" + sb.Length.ToString());
    return 0;
  end Main;
end C;
)BZL"),
                "31400|300\n");
}

// K8.1 exception edge: a later operand throws, and the Insert guard throws, after the root was produced.
BEZEL_TEST(stringbuilder_owned_call_receiver_7733, exception_edges_dispose_the_root_loop_100_spec_28_3) {
    expectClean(runAotChecked(R"BZL(
module M; import System; import System.Text;
class C
  public static method Fresh(sb: StringBuilder): owned StringBuilder
    sb.Append("R");
    return new StringBuilder("abc");
  end Fresh;
  public static method Boom(): string
    throw new Exception("x");
  end Boom;
  public static method Main(): int
    using var sb := new StringBuilder();
    var total := 0;
    for var i in 0..<100 do
      try
        total += Fresh(sb).Insert(0, Boom()).ToString().Length;
      catch Exception => total += 1;
      end try;
      try
        total += Fresh(sb).Insert(99, "x").Length;
      catch IndexOutOfRangeException => total += 2;
      end try;
      try
        total += Fresh(sb).Insert(99, "x" + "y").Length;
      catch IndexOutOfRangeException => total += 2;
      end try;
    end for;
    Console.WriteLine(total.ToString() + "|" + sb.Length.ToString());
    return 0;
  end Main;
end C;
)BZL"),
                "500|300\n");
}

// An `await` suspends inside the full expression that holds the call-rooted (and a `new`-rooted) receiver.
BEZEL_TEST(stringbuilder_owned_call_receiver_7733, await_in_the_same_full_expression_loop_100_spec_28_3) {
    expectClean(runAotChecked(R"BZL(
module M; import System; import System.Text;
class C
  public static method Fresh(sb: StringBuilder): owned StringBuilder
    sb.Append("R");
    return new StringBuilder("abc");
  end Fresh;
  public static async method Dly(n: int): Task<int>
    await Task.Delay(1);
    return n;
  end Dly;
  public static async method Run(sb: StringBuilder): Task<int>
    var t := 0;
    for var i in 0..<100 do
      t += await Dly(Fresh(sb).Insert(0, "x").Length);
      t += await Dly(new StringBuilder("zz").Insert(0, "x").Length);
    end for;
    return t;
  end Run;
  public static async method Main(): Task
    using var sb := new StringBuilder();
    var r := await Run(sb);
    Console.WriteLine(r.ToString() + "|" + sb.Length.ToString());
  end Main;
end C;
)BZL"),
                "700|100\n");
}
