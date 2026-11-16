// Regression test for issue #8501 -- the DIRECT `?.` call of a built-in
// COLLECTION PROCEDURE (`arr?.Add(5)`, `arr?.Sort()`, `arr?.Clear()`,
// `d?.Add(k, v)`, `s?.Add(x)`, `q?.Enqueue(x)`, `k?.Push(x)`) was SILENTLY
// DROPPED over a NON-null receiver.
//
// THE DEFECT.
//
//   spec §7.5 (v0.357), *Evaluation*, verbatim:
//
//     "`?.` short-circuits. The receiver is evaluated first; if it is null
//     the access does not occur and nothing to its right in the chain is
//     evaluated -- no member is read, no call is performed."
//
//   The receiver here is NOT null, so the call IS performed -- and §7.5's own
//   example of the access it performs is a call with NO value:
//
//     "user?.Log("Accessed");"
//
//   So a procedure is a §7.5 population: over a non-null receiver the
//   procedure runs, and its result is "no value" (the `.` spelling of a
//   procedure call yields none either -- stdlib §4.4 declares `Array<T>.Add`
//   / `Sort` / `Clear` as unit procedures).
//
//   Before the fix the `?.` call of such a procedure compiled, emitted
//   NOTHING, and the program answered wrongly at exit 0 -- no diagnostic, no
//   `internal:` note. MEASURED on the base (issue #8501's repro):
//
//     count after add: 3      (expected 4)
//     sorted first: 3         (expected 1)
//     count after clear: 3    (expected 0)
//
//   The `.` spelling of the same three calls printed 4 / 1 / 0.
//
// WHERE IT WAS LOST. codegen only: `lowerNullCondBuiltinCall` refused a call
// whose sema type is not a nullable `T?`, and a built-in procedure reached
// through `?.` has NO sema type at all (a built-in instance member carries no
// MethodSymbol, and the `.` twin sema types is untyped too -- #2756), so the
// whole `?.` call was declined and lowered to nothing.
//
// THE ORACLE.
//
//   The rows assert the EXACT stdout of the AOT program -- the effect of the
//   call, byte for byte. Exit 0 alone is NOT an oracle: the pre-fix program
//   exits 0 too, with the wrong answer.
//
//   The CONTROL rows assert that the VALUE-returning `?.` builtins
//   (`a?.Contains(2)`, `a?.Count`) and the plain `.` spelling of the same
//   procedures are unaffected -- they pass on BOTH arms, so a "lower nothing"
//   or "lower everything" regression cannot pass.
//
//   The NULL-RECEIVER row pins the short-circuit: over a null receiver the
//   procedure does NOT run and its ARGUMENT is not evaluated either (§7.5:
//   "nothing to its right in the chain is evaluated" -- the argument
//   expressions of the skipped suffix are not evaluated either).
//
//   The MEMORY row runs the mutating `?.` call under the ASan/LSan net
//   (`runAotChecked`), so a double-dispose or a leak introduced by the new
//   lowering fails here.
//
// Cite: spec §7.5 (v0.357), §7.4 (v0.357), §4.7 (v0.357); stdlib §4.4, §4.8,
// §4.10, §4.11 (v0.357).

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotOut / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// The issue's repro program, VERBATIM (issue #8501, `probes/
// b01_direct_void_builtin.bzl`), with the expected stdout.
constexpr const char* kIssueRepro = R"bzl(
module Conf;

public class Program
  public static method Main()
    using var arr: Array<int>? := [3, 1, 2];
    arr?.Add(5);
    Console.WriteLine($"count after add: {arr!.Count}");
    arr?.Sort();
    Console.WriteLine($"sorted first: {arr![0]}");
    arr?.Clear();
    Console.WriteLine($"count after clear: {arr!.Count}");
    using var n: Array<int>? := null;
    n?.Add(1);
    Console.WriteLine("done");
  end Main;
end Program;
)bzl";

}  // namespace

// The issue's own repro, verbatim: the three `Array<T>` procedures run over a
// non-null receiver, and the null receiver's `Add` does not.
BEZEL_TEST(null_cond_direct_void_builtin_8501,
           issue_repro_array_procedures_run_on_the_non_null_path_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(kIssueRepro),
                    "count after add: 4\nsorted first: 1\n"
                    "count after clear: 0\ndone");
}

// The same defect on the OTHER built-in collections (issue #8501's table):
// `Dictionary.Add`, `Set.Add`, `Queue.Enqueue`, `Stack.Push`. The counts
// below are the `.` spelling's answers (the control row below pins them
// independently).
BEZEL_TEST(null_cond_direct_void_builtin_8501,
           other_builtin_collections_procedures_run_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Program
  public static method Main()
    using var d: Dictionary<string, int>? := ["k": 1];
    d?.Add("k2", 2);
    Console.WriteLine($"dict count: {d!.Count}");
    using var s: Set<int>? := [1];
    s?.Add(4);
    Console.WriteLine($"set count: {s!.Count}");
    using var q: Queue<int>? := [1];
    q?.Enqueue(4);
    Console.WriteLine($"queue count: {q!.Count}");
    using var k: Stack<int>? := [1];
    k?.Push(4);
    Console.WriteLine($"stack count: {k!.Count}");
  end Main;
end Program;
)bzl"),
                    "dict count: 2\nset count: 2\nqueue count: 2\nstack count: 2");
}

// CONTROL -- the plain `.` spelling of the same procedures: unchanged by this
// fix (it was already correct on the base).
BEZEL_TEST(null_cond_direct_void_builtin_8501,
           control_dot_spelling_unchanged_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Program
  public static method Main()
    using var a2: Array<int> := [3, 1, 2];
    a2.Add(5);
    Console.WriteLine($"count after add: {a2.Count}");
    using var a3: Array<int> := [3, 1, 2];
    a3.Sort();
    Console.WriteLine($"sorted first: {a3[0]}");
    using var a4: Array<int> := [3, 1, 2];
    a4.Clear();
    Console.WriteLine($"count after clear: {a4.Count}");
    using var d: Dictionary<string, int> := ["k": 1];
    d.Add("k2", 2);
    Console.WriteLine($"dict count: {d.Count}");
    using var s: Set<int> := [1];
    s.Add(4);
    Console.WriteLine($"set count: {s.Count}");
    using var q: Queue<int> := [1];
    q.Enqueue(4);
    Console.WriteLine($"queue count: {q.Count}");
    using var k: Stack<int> := [1];
    k.Push(4);
    Console.WriteLine($"stack count: {k.Count}");
  end Main;
end Program;
)bzl"),
                    "count after add: 4\nsorted first: 1\ncount after clear: 0\n"
                    "dict count: 2\nset count: 2\nqueue count: 2\nstack count: 2");
}

// CONTROL -- the VALUE-returning `?.` builtins: unchanged by this fix.
BEZEL_TEST(null_cond_direct_void_builtin_8501,
           control_value_returning_nullcond_builtins_unchanged_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Program
  public static method Main()
    using var a1: Array<int>? := [3, 1, 2];
    Console.WriteLine($"contains 2: {a1?.Contains(2)}");
    Console.WriteLine($"count: {a1?.Count}");
    using var sb: StringBuilder? := new StringBuilder();
    sb?.Append("xy");
    Console.WriteLine($"sb: {sb!.ToString()}");
    using var l: LinkedList<int>? := new LinkedList<int>();
    l?.AddLast(3);
    Console.WriteLine($"ll count: {l!.Count}");
  end Main;
end Program;
)bzl"),
                    "contains 2: True\ncount: 3\nsb: xy\nll count: 1");
}

// §7.5's short-circuit: over a NULL receiver the procedure does not run, and
// its ARGUMENT is not evaluated either ("nothing to its right in the chain is
// evaluated" -- the argument expressions of the skipped suffix are not
// evaluated either). The argument here has an observable effect, so the row
// pins the evaluation as well as the call.
BEZEL_TEST(null_cond_direct_void_builtin_8501,
           null_receiver_skips_the_call_and_its_argument_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Helper
  public static method Next(): int
    Console.WriteLine("arg evaluated");
    return 7;
  end Next;
end Helper;

public class Program
  public static method Main()
    using var none: Array<int>? := null;
    none?.Add(Helper.Next());
    Console.WriteLine("after null");
    using var some: Array<int>? := [1];
    some?.Add(Helper.Next());
    Console.WriteLine($"count: {some!.Count}");
  end Main;
end Program;
)bzl"),
                    "after null\narg evaluated\ncount: 2");
}

// The MEMORY net: the mutating `?.` call over a non-null receiver runs under
// ASan/LSan, so a double-dispose or a leak in the new lowering fails here.
BEZEL_TEST(null_cond_direct_void_builtin_8501,
           mutating_nullcond_procedure_is_memory_clean_7_5) {
    JitResult r = runAotChecked(R"bzl(
module Conf;

public class Program
  public static method Main()
    using var arr: Array<int>? := [3, 1, 2];
    arr?.Add(5);
    arr?.Sort();
    Console.WriteLine($"sorted first: {arr![0]}");
    arr?.Clear();
    Console.WriteLine($"count after clear: {arr!.Count}");
  end Main;
end Program;
)bzl");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);  // fails if ASan/LSan fires
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "sorted first: 1\ncount after clear: 0\n");
}
