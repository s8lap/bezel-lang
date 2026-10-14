// Codegen / soundness - issue #6052 (spec §30.5 kind 1, v0.338): "A fresh
// owned value (a `return` of an owned expression - a fresh producer, or a
// named owned binding of the body, a plain owned local or an `owned`
// parameter, consumed marker-free at the return ...): the task owns the
// result and its destructor disposes it directly".
//
// In an `async` body with more than one `return`, a local that is returned on
// one path and still owned at another exit is flagged for the §28.3 scope-end
// auto-drop, so it sits on the frame's disposal stack. The async return edge
// ran those disposals over the very local it was returning: the frame freed
// the object, the task's result slot dangled, and the task's teardown (or the
// awaiter) touched freed memory - heap-use-after-free in `bz$elemDispose`
// under `Task<class>` / `Task<interface>`, and in the awaiter under
// `OwningTask<T>`.
//
// Every row runs its calls 25 times under the always-ASan `runAotChecked`
// oracle (LeakSanitizer on) and pins the destructor trace of EACH call: the
// returned object is destroyed exactly once, by the result's owner and after
// the caller read it (`gotN` precedes `DN`), and every local the path did not
// return is destroyed exactly once by the frame.
#include <string>
#include <utility>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

constexpr int kIterations = 25;

constexpr const char* kTypes =
    "module M; "
    "public interface IShape method Id(): int; end IShape; "
    "public class Dq : IShape "
    "  public id: int; "
    "  public constructor(i: int) this.id := i; end constructor; "
    "  public method Id(): int return this.id; end Id; "
    "  destructor Console.WriteLine(\"D\" + this.id.ToString()); "
    "  end destructor; "
    "end Dq; ";

// One call's expected lines: the space-separated trace, then the separator.
std::string traceLines(const std::string& trace) {
    std::string out;
    std::string word;
    for (char ch : trace + " ") {
        if (ch == ' ') {
            if (!word.empty()) out += word + "\n";
            word.clear();
        } else {
            word += ch;
        }
    }
    return out + "--\n";
}

using Calls = std::vector<std::pair<std::string, std::string>>;

// `F(<params>): <retType>` with `body`, called once per entry of `calls`
// (argument list, expected trace) on every iteration. `owning` selects the
// §30.5 Pattern B consumption (`using var r := await F(..)`) over the Pattern
// A one (`using var t := F(..); var r := await t;`); `read` renders the id of
// the awaited result `%s`.
std::string program(const std::string& retType, const std::string& params,
                    const std::string& body, const Calls& calls, bool owning,
                    const std::string& read, bool async = true) {
    std::string s = kTypes;
    s += "public class Program ";
    s += "public static async method Tick(): Task<int> return 1; end Tick; ";
    s += std::string("public static ") + (async ? "async " : "") +
         "method F(" + params + "): " + retType + " " + body + " end F; ";
    s += async ? "public static async method Main(): Task<int> "
               : "public static method Main(): int ";
    s += "var i: int := 0; while i < " + std::to_string(kIterations) + " do ";
    for (const auto& c : calls) {
        std::string rd = read;
        rd.replace(rd.find("%s"), 2, "r");
        if (!async)
            s += "do using var r := F(" + c.first + "); ";
        else if (owning)
            s += "do using var r := await F(" + c.first + "); ";
        else
            s += "do using var t := F(" + c.first + "); var r := await t; ";
        s += "Console.WriteLine(\"got\" + " + rd + "); end; "
             "Console.WriteLine(\"--\"); ";
    }
    s += "i := i + 1; end while; return 0; end Main; end Program;";
    return s;
}

std::string expected(const Calls& calls) {
    std::string once;
    for (const auto& c : calls) once += traceLines(c.second);
    std::string all;
    for (int i = 0; i < kIterations; ++i) all += once;
    return all;
}

void expectExactlyOnce(const std::string& retType, const std::string& params,
                       const std::string& body, const Calls& calls,
                       bool owning, const std::string& read,
                       bool async = true) {
    JitResult r = runAotChecked(
        program(retType, params, body, calls, owning, read, async));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected(calls));
}

// The §28.3 *return slot* rows: the same body under each of the three result
// forms, every call wrapped in a handler so a return the edge abandons shows
// as `exc`. `calls` pairs an argument with the exact trace of that call.
void expectTracesUnderEveryResultForm(const std::string& body,
                                      const Calls& calls) {
    struct Form {
        const char* retType;
        bool owning;
        const char* read;
    };
    const Form forms[] = {
        {"Task<Dq>", false, "r.id.ToString()"},
        {"Task<IShape>", false, "r.Id().ToString()"},
        {"OwningTask<Dq>", true, "r.id.ToString()"},
    };
    for (const Form& f : forms) {
        std::string s = kTypes;
        s += "public class Boom "
             "  public id: int; "
             "  public constructor(i: int) this.id := i; end constructor; "
             "  destructor Console.WriteLine(\"B\" + this.id.ToString()); "
             "    if this.id = 0 then "
             "      throw new InvalidOperationException(\"boom\"); end if; "
             "  end destructor; "
             "end Boom; "
             "public class Program "
             "public static method Thrower(n: int): int "
             "  if n = 0 then throw new InvalidOperationException(\"t\"); "
             "  end if; return n; end Thrower; ";
        s += std::string("public static async method F(n: int): ") +
             f.retType + " " + body + " end F; ";
        s += "public static async method Main(): Task<int> "
             "var i: int := 0; while i < " +
             std::to_string(kIterations) + " do ";
        for (const auto& c : calls) {
            s += "try ";
            s += f.owning
                     ? "do using var r := await F(" + c.first + "); "
                     : "do using var t := F(" + c.first +
                           "); var r := await t; ";
            s += std::string("Console.WriteLine(\"got\" + ") + f.read +
                 "); end; "
                 "catch var ex: Exception => Console.WriteLine(\"exc\"), "
                 "end try; Console.WriteLine(\"--\"); ";
        }
        s += "i := i + 1; end while; return 0; end Main; end Program;";
        JitResult r = runAotChecked(s);
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, expected(calls));
    }
}

}  // namespace

// The issue's repro: each call returns one local and scope-disposes the other.
BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_two_owned_locals_returned_on_different_paths_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o: Dq := new Dq(1); "
        "var q: Dq := new Dq(2); "
        "if c then return q; end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "D2 got1 D1"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_two_owned_locals_returned_on_different_paths_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o: Dq := new Dq(1); "
        "var q: Dq := new Dq(2); "
        "if c then return q; end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "D2 got1 D1"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_two_owned_locals_returned_on_different_paths_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o: Dq := new Dq(1); "
        "var q: Dq := new Dq(2); "
        "if c then return q; end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "D2 got1 D1"}},
        true, "%s.id.ToString()");
}

// A local returned on one path and scope-disposed on the other.
BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_local_returned_in_if_else_fresh_value_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o := new Dq(1); "
        "if c then return o; end if; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_local_returned_in_if_else_fresh_value_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o := new Dq(1); "
        "if c then return o; end if; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_local_returned_in_if_else_fresh_value_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o := new Dq(1); "
        "if c then return o; end if; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        true, "%s.id.ToString()");
}

// The LAST return is the one that returns the local.
BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_fresh_value_in_if_local_returned_last_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o := new Dq(1); "
        "if c then return new Dq(2); end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "got1 D1"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_fresh_value_in_if_local_returned_last_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o := new Dq(1); "
        "if c then return new Dq(2); end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "got1 D1"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_fresh_value_in_if_local_returned_last_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o := new Dq(1); "
        "if c then return new Dq(2); end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "got1 D1"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_local_returned_inside_while_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o := new Dq(1); "
        "var k := 0; "
        "while k < 3 do "
        "  k := k + 1; "
        "  if c and k = 2 then return o; end if; "
        "end while; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_local_returned_inside_while_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o := new Dq(1); "
        "var k := 0; "
        "while k < 3 do "
        "  k := k + 1; "
        "  if c and k = 2 then return o; end if; "
        "end while; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_local_returned_inside_while_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o := new Dq(1); "
        "var k := 0; "
        "while k < 3 do "
        "  k := k + 1; "
        "  if c and k = 2 then return o; end if; "
        "end while; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_local_returned_inside_for_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o := new Dq(1); "
        "for var k in 0..<3 do "
        "  if c and k = 1 then return o; end if; "
        "end for; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_local_returned_inside_for_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o := new Dq(1); "
        "for var k in 0..<3 do "
        "  if c and k = 1 then return o; end if; "
        "end for; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_local_returned_inside_for_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o := new Dq(1); "
        "for var k in 0..<3 do "
        "  if c and k = 1 then return o; end if; "
        "end for; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_locals_returned_from_case_arms_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "n: int",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "case n of "
        "  0 => do return o; end, "
        "  1 => do return q; end, "
        "  _ => Console.WriteLine(\"other\") "
        "end case; "
        "return new Dq(3); ",
        {{"0", "D2 got1 D1"}, {"1", "D1 got2 D2"}, {"2", "other D2 D1 got3 D3"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_locals_returned_from_case_arms_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "n: int",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "case n of "
        "  0 => do return o; end, "
        "  1 => do return q; end, "
        "  _ => Console.WriteLine(\"other\") "
        "end case; "
        "return new Dq(3); ",
        {{"0", "D2 got1 D1"}, {"1", "D1 got2 D2"}, {"2", "other D2 D1 got3 D3"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_locals_returned_from_case_arms_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "n: int",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "case n of "
        "  0 => do return o; end, "
        "  1 => do return q; end, "
        "  _ => Console.WriteLine(\"other\") "
        "end case; "
        "return new Dq(3); ",
        {{"0", "D2 got1 D1"}, {"1", "D1 got2 D2"}, {"2", "other D2 D1 got3 D3"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_outer_local_returned_from_nested_blocks_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o := new Dq(1); "
        "do "
        "  var q := new Dq(2); "
        "  do "
        "    if c then return o; end if; "
        "  end; "
        "  Console.WriteLine(\"mid\" + q.id.ToString()); "
        "end; "
        "return new Dq(3); ",
        {{"true", "D2 got1 D1"}, {"false", "mid2 D2 D1 got3 D3"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_outer_local_returned_from_nested_blocks_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o := new Dq(1); "
        "do "
        "  var q := new Dq(2); "
        "  do "
        "    if c then return o; end if; "
        "  end; "
        "  Console.WriteLine(\"mid\" + q.id.ToString()); "
        "end; "
        "return new Dq(3); ",
        {{"true", "D2 got1 D1"}, {"false", "mid2 D2 D1 got3 D3"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_outer_local_returned_from_nested_blocks_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o := new Dq(1); "
        "do "
        "  var q := new Dq(2); "
        "  do "
        "    if c then return o; end if; "
        "  end; "
        "  Console.WriteLine(\"mid\" + q.id.ToString()); "
        "end; "
        "return new Dq(3); ",
        {{"true", "D2 got1 D1"}, {"false", "mid2 D2 D1 got3 D3"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_inner_local_returned_then_outer_local_returned_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o := new Dq(1); "
        "if c then "
        "  var q := new Dq(2); "
        "  return q; "
        "end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "got1 D1"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_inner_local_returned_then_outer_local_returned_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o := new Dq(1); "
        "if c then "
        "  var q := new Dq(2); "
        "  return q; "
        "end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "got1 D1"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_inner_local_returned_then_outer_local_returned_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o := new Dq(1); "
        "if c then "
        "  var q := new Dq(2); "
        "  return q; "
        "end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "got1 D1"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_three_locals_if_elif_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "n: int",
        "var a := new Dq(1); "
        "var b := new Dq(2); "
        "var d := new Dq(3); "
        "if n = 0 then return a; elif n = 1 then return b; end if; "
        "return d; ",
        {{"0", "D3 D2 got1 D1"}, {"1", "D3 D1 got2 D2"}, {"2", "D2 D1 got3 D3"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_three_locals_if_elif_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "n: int",
        "var a := new Dq(1); "
        "var b := new Dq(2); "
        "var d := new Dq(3); "
        "if n = 0 then return a; elif n = 1 then return b; end if; "
        "return d; ",
        {{"0", "D3 D2 got1 D1"}, {"1", "D3 D1 got2 D2"}, {"2", "D2 D1 got3 D3"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_three_locals_if_elif_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "n: int",
        "var a := new Dq(1); "
        "var b := new Dq(2); "
        "var d := new Dq(3); "
        "if n = 0 then return a; elif n = 1 then return b; end if; "
        "return d; ",
        {{"0", "D3 D2 got1 D1"}, {"1", "D3 D1 got2 D2"}, {"2", "D2 D1 got3 D3"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_same_local_returned_on_two_paths_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "if c then return o; end if; "
        "Console.WriteLine(\"x\" + q.id.ToString()); "
        "return o; ",
        {{"true", "D2 got1 D1"}, {"false", "x2 D2 got1 D1"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_same_local_returned_on_two_paths_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "if c then return o; end if; "
        "Console.WriteLine(\"x\" + q.id.ToString()); "
        "return o; ",
        {{"true", "D2 got1 D1"}, {"false", "x2 D2 got1 D1"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_same_local_returned_on_two_paths_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "if c then return o; end if; "
        "Console.WriteLine(\"x\" + q.id.ToString()); "
        "return o; ",
        {{"true", "D2 got1 D1"}, {"false", "x2 D2 got1 D1"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_nullable_local_returned_through_bang_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o: Dq? := new Dq(1); "
        "if c then return o!; end if; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_nullable_local_returned_through_bang_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o: Dq? := new Dq(1); "
        "if c then return o!; end if; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_nullable_local_returned_through_bang_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o: Dq? := new Dq(1); "
        "if c then return o!; end if; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "D1 got2 D2"}},
        true, "%s.id.ToString()");
}

// The enclosing scopes are disposed by the finally's mode-5 dispatch, shared
// by every return routed through it: the choice is made at run time.
BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_returns_routed_through_finally_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "n: int",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "try "
        "  if n = 0 then return o; end if; "
        "  if n = 1 then return q; end if; "
        "finally "
        "  Console.WriteLine(\"fin\"); "
        "end try; "
        "return new Dq(3); ",
        {{"0", "fin D2 got1 D1"}, {"1", "fin D1 got2 D2"}, {"2", "fin D2 D1 got3 D3"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_returns_routed_through_finally_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "n: int",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "try "
        "  if n = 0 then return o; end if; "
        "  if n = 1 then return q; end if; "
        "finally "
        "  Console.WriteLine(\"fin\"); "
        "end try; "
        "return new Dq(3); ",
        {{"0", "fin D2 got1 D1"}, {"1", "fin D1 got2 D2"}, {"2", "fin D2 D1 got3 D3"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_returns_routed_through_finally_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "n: int",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "try "
        "  if n = 0 then return o; end if; "
        "  if n = 1 then return q; end if; "
        "finally "
        "  Console.WriteLine(\"fin\"); "
        "end try; "
        "return new Dq(3); ",
        {{"0", "fin D2 got1 D1"}, {"1", "fin D1 got2 D2"}, {"2", "fin D2 D1 got3 D3"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_try_body_local_returned_through_finally_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o := new Dq(1); "
        "try "
        "  var q := new Dq(2); "
        "  if c then return q; end if; "
        "  Console.WriteLine(\"body\" + q.id.ToString()); "
        "finally "
        "  Console.WriteLine(\"fin\"); "
        "end try; "
        "return o; ",
        {{"true", "fin D1 got2 D2"}, {"false", "body2 D2 fin got1 D1"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_try_body_local_returned_through_finally_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o := new Dq(1); "
        "try "
        "  var q := new Dq(2); "
        "  if c then return q; end if; "
        "  Console.WriteLine(\"body\" + q.id.ToString()); "
        "finally "
        "  Console.WriteLine(\"fin\"); "
        "end try; "
        "return o; ",
        {{"true", "fin D1 got2 D2"}, {"false", "body2 D2 fin got1 D1"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_try_body_local_returned_through_finally_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o := new Dq(1); "
        "try "
        "  var q := new Dq(2); "
        "  if c then return q; end if; "
        "  Console.WriteLine(\"body\" + q.id.ToString()); "
        "finally "
        "  Console.WriteLine(\"fin\"); "
        "end try; "
        "return o; ",
        {{"true", "fin D1 got2 D2"}, {"false", "body2 D2 fin got1 D1"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_returns_routed_through_nested_finally_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "n: int",
        "var o := new Dq(1); "
        "try "
        "  var q := new Dq(2); "
        "  try "
        "    if n = 0 then return o; end if; "
        "    if n = 1 then return q; end if; "
        "  finally "
        "    Console.WriteLine(\"fin1\"); "
        "  end try; "
        "finally "
        "  Console.WriteLine(\"fin2\"); "
        "end try; "
        "return new Dq(3); ",
        {{"0", "fin1 D2 fin2 got1 D1"}, {"1", "fin1 fin2 D1 got2 D2"}, {"2", "fin1 D2 fin2 D1 got3 D3"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_returns_routed_through_nested_finally_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "n: int",
        "var o := new Dq(1); "
        "try "
        "  var q := new Dq(2); "
        "  try "
        "    if n = 0 then return o; end if; "
        "    if n = 1 then return q; end if; "
        "  finally "
        "    Console.WriteLine(\"fin1\"); "
        "  end try; "
        "finally "
        "  Console.WriteLine(\"fin2\"); "
        "end try; "
        "return new Dq(3); ",
        {{"0", "fin1 D2 fin2 got1 D1"}, {"1", "fin1 fin2 D1 got2 D2"}, {"2", "fin1 D2 fin2 D1 got3 D3"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_returns_routed_through_nested_finally_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "n: int",
        "var o := new Dq(1); "
        "try "
        "  var q := new Dq(2); "
        "  try "
        "    if n = 0 then return o; end if; "
        "    if n = 1 then return q; end if; "
        "  finally "
        "    Console.WriteLine(\"fin1\"); "
        "  end try; "
        "finally "
        "  Console.WriteLine(\"fin2\"); "
        "end try; "
        "return new Dq(3); ",
        {{"0", "fin1 D2 fin2 got1 D1"}, {"1", "fin1 fin2 D1 got2 D2"}, {"2", "fin1 D2 fin2 D1 got3 D3"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_local_returned_inside_try_catch_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o := new Dq(1); "
        "try "
        "  if c then return o; end if; "
        "  Console.WriteLine(\"body\"); "
        "catch "
        "  var ex: Exception => Console.WriteLine(\"caught\"), "
        "end try; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "body D1 got2 D2"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_local_returned_inside_try_catch_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o := new Dq(1); "
        "try "
        "  if c then return o; end if; "
        "  Console.WriteLine(\"body\"); "
        "catch "
        "  var ex: Exception => Console.WriteLine(\"caught\"), "
        "end try; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "body D1 got2 D2"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_local_returned_inside_try_catch_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o := new Dq(1); "
        "try "
        "  if c then return o; end if; "
        "  Console.WriteLine(\"body\"); "
        "catch "
        "  var ex: Exception => Console.WriteLine(\"caught\"), "
        "end try; "
        "return new Dq(2); ",
        {{"true", "got1 D1"}, {"false", "body D1 got2 D2"}},
        true, "%s.id.ToString()");
}

// A body that suspends may not rely on the auto-drop (§30.1), so the other
// path disposes explicitly; `u` is the scope-bound control.
BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_local_returned_after_await_suspension_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "using var u := new Dq(9); "
        "var o := new Dq(1); "
        "var k := await Tick(); "
        "if c then return o; end if; "
        "o.Dispose(); "
        "return new Dq(2); ",
        {{"true", "D9 got1 D1"}, {"false", "D1 D9 got2 D2"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_local_returned_after_await_suspension_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "using var u := new Dq(9); "
        "var o := new Dq(1); "
        "var k := await Tick(); "
        "if c then return o; end if; "
        "o.Dispose(); "
        "return new Dq(2); ",
        {{"true", "D9 got1 D1"}, {"false", "D1 D9 got2 D2"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_local_returned_after_await_suspension_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "using var u := new Dq(9); "
        "var o := new Dq(1); "
        "var k := await Tick(); "
        "if c then return o; end if; "
        "o.Dispose(); "
        "return new Dq(2); ",
        {{"true", "D9 got1 D1"}, {"false", "D1 D9 got2 D2"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_two_locals_after_await_suspension_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "c: bool",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "var k := await Tick(); "
        "if c then q.Dispose(); return o; end if; "
        "o.Dispose(); "
        "return q; ",
        {{"true", "D2 got1 D1"}, {"false", "D1 got2 D2"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_two_locals_after_await_suspension_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "c: bool",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "var k := await Tick(); "
        "if c then q.Dispose(); return o; end if; "
        "o.Dispose(); "
        "return q; ",
        {{"true", "D2 got1 D1"}, {"false", "D1 got2 D2"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_two_locals_after_await_suspension_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "c: bool",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "var k := await Tick(); "
        "if c then q.Dispose(); return o; end if; "
        "o.Dispose(); "
        "return q; ",
        {{"true", "D2 got1 D1"}, {"false", "D1 got2 D2"}},
        true, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_throw_on_a_third_path_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "n: int",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "if n = 0 then return q; end if; "
        "if n = 1 then throw new InvalidOperationException(\"x\"); end if; "
        "return o; ",
        {{"0", "D1 got2 D2"}, {"2", "D2 got1 D1"}},
        false, "%s.id.ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          task_interface_throw_on_a_third_path_30_5) {
    expectExactlyOnce(
        "Task<IShape>", "n: int",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "if n = 0 then return q; end if; "
        "if n = 1 then throw new InvalidOperationException(\"x\"); end if; "
        "return o; ",
        {{"0", "D1 got2 D2"}, {"2", "D2 got1 D1"}},
        false, "%s.Id().ToString()");
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          owning_task_throw_on_a_third_path_30_5) {
    expectExactlyOnce(
        "OwningTask<Dq>", "n: int",
        "var o := new Dq(1); "
        "var q := new Dq(2); "
        "if n = 0 then return q; end if; "
        "if n = 1 then throw new InvalidOperationException(\"x\"); end if; "
        "return o; ",
        {{"0", "D1 got2 D2"}, {"2", "D2 got1 D1"}},
        true, "%s.id.ToString()");
}

// An `owned` parameter returned on one path and disposed on the other, next
// to a local with the mirrored fate (control: the parameter's frame capture
// has its own null-guarded auto-dispose).
BEZEL_TEST(async_multi_return_owned_local_6052,
          task_class_owned_param_and_local_swap_fates_30_5) {
    expectExactlyOnce(
        "Task<Dq>", "owned p: Dq, c: bool",
        "var o := new Dq(2); "
        "if c then o.Dispose(); return p; end if; "
        "p.Dispose(); "
        "return o; ",
        {{"new Dq(1), true", "D2 got1 D1"}, {"new Dq(1), false", "D1 got2 D2"}},
        false, "%s.id.ToString()");
}

// A nullable generic-class local and a non-nullable one (no destructor: the
// oracle here is ASan + LeakSanitizer over the loop).
BEZEL_TEST(async_multi_return_owned_local_6052,
          task_nullable_generic_class_two_locals_looped_30_5) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Box<T> public v: T; "
        "  public constructor(x: T) this.v := x; end constructor; end Box; "
        "public class Program "
        "public static async method F(c: bool): Task<Box<int>?> "
        "  var o: Box<int>? := new Box<int>(1); "
        "  var q := new Box<int>(2); "
        "  if c then return q; end if; "
        "  return o; "
        "end F; "
        "public static async method Main(): Task<int> "
        "  var i: int := 0; var n: int := 0; "
        "  while i < 200 do "
        "    do using var t := F(i mod 2 = 0); var r := await t; "
        "       n := n + r!.v; end; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(n); return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

// A returned local a lambda captured (§27.5) lives in a capture cell. The
// return moves the object out of the cell and leaves the cell to the frame:
// nulling the cell holder instead leaked the cell on every call.
BEZEL_TEST(async_multi_return_owned_local_6052,
          captured_local_single_return_keeps_its_capture_cell_27_5) {
    expectTracesUnderEveryResultForm(
        "var o := new Dq(1); "
        "var f := lambda() => o.id; "
        "Console.WriteLine(f()); "
        "return o;",
        {{"0", "1 got1 D1"}});
}

// The same captured local with a second `return`: scope-disposed on the path
// that does not return it, the task's on the path that does.
BEZEL_TEST(async_multi_return_owned_local_6052,
          captured_local_returned_on_one_path_27_5) {
    expectTracesUnderEveryResultForm(
        "var o := new Dq(1); "
        "var f := lambda() => o.id; "
        "Console.WriteLine(f()); "
        "if n = 0 then return o; end if; "
        "return new Dq(3);",
        {{"0", "1 got1 D1"}, {"1", "1 D1 got3 D3"}});
}

// The first `return` runs before the capture cell is allocated.
BEZEL_TEST(async_multi_return_owned_local_6052,
          captured_local_returned_before_and_after_the_capture_27_5) {
    expectTracesUnderEveryResultForm(
        "var o := new Dq(1); "
        "if n = 0 then return o; end if; "
        "var f := lambda() => o.id; "
        "Console.WriteLine(f()); "
        "if n = 1 then return o; end if; "
        "return new Dq(3);",
        {{"0", "got1 D1"}, {"1", "1 got1 D1"}});
}

// A captured local returned through a `finally`.
BEZEL_TEST(async_multi_return_owned_local_6052,
          captured_local_returned_through_finally_27_5) {
    expectTracesUnderEveryResultForm(
        "var o := new Dq(1); "
        "var f := lambda() => o.id; "
        "try "
        "Console.WriteLine(f()); "
        "if n = 0 then return o; end if; "
        "finally "
        "Console.WriteLine(\"fin\"); "
        "end try; "
        "return new Dq(3);",
        {{"0", "1 fin got1 D1"}, {"1", "1 fin D1 got3 D3"}});
}

// A captured try-body local whose `finally` raises on the return edge.
BEZEL_TEST(async_multi_return_owned_local_6052,
          captured_try_local_finally_throws_28_3) {
    expectTracesUnderEveryResultForm(
        "try "
        "var o := new Dq(1); "
        "var f := lambda() => o.id; "
        "Console.WriteLine(f()); "
        "if n < 2 then return o; end if; "
        "finally "
        "if n = 0 then throw new InvalidOperationException(\"f\"); end if; "
        "end try; "
        "return new Dq(3);",
        {{"0", "1 D1 exc"}, {"1", "1 got1 D1"}, {"2", "1 D1 got3 D3"}});
}

// §28.3 *The return slot* (v0.338): "if one raises - a `finally` that throws
// ... - the return is abandoned ... and the slot's value is disposed by the
// unwind ... at the raise". The returned try-body local is destroyed exactly
// once, the task faults, and the awaiter sees the exception.
BEZEL_TEST(async_multi_return_owned_local_6052,
          finally_throws_on_the_return_edge_disposes_the_slot_28_3) {
    expectTracesUnderEveryResultForm(
        "try "
        "var o := new Dq(1); "
        "if n = 0 then return o; end if; "
        "finally "
        "if n = 0 then throw new InvalidOperationException(\"f\"); end if; "
        "end try; "
        "return new Dq(3);",
        {{"0", "D1 exc"}, {"1", "D1 got3 D3"}});
}

// One `return` only: the local is not on the disposal stack's normal edge at
// all, so nothing but the return slot can dispose it when the finally raises.
BEZEL_TEST(async_multi_return_owned_local_6052,
          single_return_finally_throws_disposes_the_slot_28_3) {
    expectTracesUnderEveryResultForm(
        "try "
        "var o := new Dq(1); "
        "return o; "
        "finally "
        "if n = 0 then throw new InvalidOperationException(\"f\"); end if; "
        "end try;",
        {{"0", "D1 exc"}, {"1", "got1 D1"}});
}

// Two finallies on the route, the OUTER one raises. "at the raise, before
// every cleanup still pending on the edge that was established before the
// `return` statement": the returned local is destroyed when the outer finally
// raises - after the inner finally and the scopes between the two (`D2`),
// before the outermost local (`D1`). One arm per returned local.
BEZEL_TEST(async_multi_return_owned_local_6052,
          outer_finally_throws_after_inner_finally_completed_28_3) {
    expectTracesUnderEveryResultForm(
        "var a := new Dq(1); "
        "try "
        "var b := new Dq(2); "
        "try "
        "var c := new Dq(3); "
        "if n = 0 then return c; end if; "
        "if n = 1 then return b; end if; "
        "if n = 2 then return a; end if; "
        "finally "
        "Console.WriteLine(\"fin1\"); "
        "end try; "
        "finally "
        "if n < 3 then throw new InvalidOperationException(\"f\"); end if; "
        "end try; "
        "return new Dq(4);",
        {{"0", "fin1 D2 D3 D1 exc"}, {"1", "D3 fin1 D2 D1 exc"}, {"2", "D3 fin1 D2 D1 exc"}, {"3", "D3 fin1 D2 D1 got4 D4"}});
}

// "so a handler that catches the exception finds no returned value left to
// dispose": the abandoned return's local is gone before `caught`, and the body
// goes on to return another local.
BEZEL_TEST(async_multi_return_owned_local_6052,
          inner_finally_throws_and_an_outer_handler_catches_28_3) {
    expectTracesUnderEveryResultForm(
        "var a := new Dq(1); "
        "try "
        "try "
        "var c := new Dq(3); "
        "if n = 0 then return c; end if; "
        "finally "
        "if n = 0 then throw new InvalidOperationException(\"f\"); end if; "
        "end try; "
        "catch "
        "var ex: Exception => Console.WriteLine(\"caught\"), "
        "end try; "
        "return a;",
        {{"0", "D3 caught got1 D1"}, {"1", "D3 got1 D1"}});
}

// Three finallies; the middle one raises into a handler inside the outermost.
BEZEL_TEST(async_multi_return_owned_local_6052,
          middle_of_three_finallies_throws_and_is_caught_28_3) {
    expectTracesUnderEveryResultForm(
        "var a := new Dq(1); "
        "try "
        "try "
        "try "
        "try "
        "var c := new Dq(3); "
        "if n < 2 then return c; end if; "
        "finally "
        "Console.WriteLine(\"fin1\"); "
        "end try; "
        "finally "
        "Console.WriteLine(\"fin2\"); "
        "if n = 0 then throw new InvalidOperationException(\"f\"); end if; "
        "end try; "
        "catch "
        "var ex: Exception => Console.WriteLine(\"caught\"), "
        "end try; "
        "finally "
        "Console.WriteLine(\"fin3\"); "
        "end try; "
        "return a;",
        {{"0", "fin1 fin2 D3 caught fin3 got1 D1"}, {"1", "fin1 fin2 fin3 D1 got3 D3"}});
}

// The finally raises through a call rather than a `throw`.
BEZEL_TEST(async_multi_return_owned_local_6052,
          finally_raise_by_a_propagating_call_28_3) {
    expectTracesUnderEveryResultForm(
        "var o := new Dq(1); "
        "try "
        "if n < 2 then return o; end if; "
        "finally "
        "Console.WriteLine(Thrower(n).ToString()); "
        "end try; "
        "return new Dq(3);",
        {{"0", "D1 exc"}, {"1", "1 got1 D1"}, {"2", "2 D1 got3 D3"}});
}

// A raise a handler INSIDE the finally body catches does not abandon the
// return: the task still gets the local.
BEZEL_TEST(async_multi_return_owned_local_6052,
          finally_raise_caught_inside_the_finally_keeps_the_return_28_3) {
    expectTracesUnderEveryResultForm(
        "var o := new Dq(1); "
        "try "
        "if n < 2 then return o; end if; "
        "finally "
        "try "
        "Console.WriteLine(Thrower(n).ToString()); "
        "catch "
        "var ex: Exception => Console.WriteLine(\"inner\"), "
        "end try; "
        "end try; "
        "return new Dq(3);",
        {{"0", "inner got1 D1"}, {"1", "1 got1 D1"}, {"2", "2 D1 got3 D3"}});
}

// "only a cleanup established inside the raising cleanup itself, such as a
// `using var` in a `finally` body, unwinds before it".
BEZEL_TEST(async_multi_return_owned_local_6052,
          cleanup_inside_the_raising_finally_unwinds_first_28_3) {
    expectTracesUnderEveryResultForm(
        "try "
        "var o := new Dq(1); "
        "if n < 2 then return o; end if; "
        "finally "
        "using var q := new Dq(7); "
        "if n = 0 then throw new InvalidOperationException(\"f\"); end if; "
        "end try; "
        "return new Dq(3);",
        {{"0", "D7 D1 exc"}, {"1", "D7 got1 D1"}});
}

// A return abandoned inside a loop, caught, and the same `try` entered again
// - where another finally raises with no return in flight. The slot must be
// empty by then: a stale value would be destroyed twice.
BEZEL_TEST(async_multi_return_owned_local_6052,
          abandoned_return_then_the_try_is_reentered_28_3) {
    expectTracesUnderEveryResultForm(
        "var k: int := 0; "
        "while k < 3 do "
        "try "
        "try "
        "try "
        "var c := new Dq(10 + k); "
        "if k = 0 or n = 1 then return c; end if; "
        "finally "
        "Console.WriteLine(\"fin1\"); "
        "if k = 1 then throw new InvalidOperationException(\"g\"); end if; "
        "end try; "
        "finally "
        "if k = 0 and n = 0 then throw new InvalidOperationException(\"f\"); end if; "
        "end try; "
        "catch "
        "var ex: Exception => Console.WriteLine(\"caught\"), "
        "end try; "
        "k := k + 1; "
        "end while; "
        "return new Dq(3);",
        {{"0", "fin1 D10 caught D11 fin1 caught D12 fin1 got3 D3"}, {"1", "fin1 got10 D10"}});
}

// §28.3 *The return slot*: "a `using var`'s or an auto-dropped local's
// disposer that throws (§28.4)" abandons the return the same way.
BEZEL_TEST(async_multi_return_owned_local_6052,
          using_var_disposer_throws_on_the_return_edge_28_3) {
    expectTracesUnderEveryResultForm(
        "var o := new Dq(1); "
        "using var u := new Boom(n); "
        "if n < 2 then return o; end if; "
        "return new Dq(3);",
        {{"0", "B0 D1 exc"}, {"1", "B1 got1 D1"}});
}

// The raising disposer belongs to the try body, so the slot's value goes
// before the finally body runs on the unwind.
BEZEL_TEST(async_multi_return_owned_local_6052,
          try_body_disposer_throws_before_the_finally_28_3) {
    expectTracesUnderEveryResultForm(
        "var o := new Dq(1); "
        "try "
        "using var u := new Boom(n); "
        "if n < 2 then return o; end if; "
        "finally "
        "Console.WriteLine(\"fin\"); "
        "end try; "
        "return new Dq(3);",
        {{"0", "B0 D1 fin exc"}, {"1", "B1 fin got1 D1"}});
}

// The raising disposer belongs to a scope enclosing the `try`: it runs after
// the finally completed normally, with the return still in flight.
BEZEL_TEST(async_multi_return_owned_local_6052,
          enclosing_disposer_throws_after_the_finally_28_3) {
    expectTracesUnderEveryResultForm(
        "using var u := new Boom(n); "
        "var o := new Dq(1); "
        "try "
        "if n < 2 then return o; end if; "
        "finally "
        "Console.WriteLine(\"fin\"); "
        "end try; "
        "return new Dq(3);",
        {{"0", "fin B0 D1 exc"}, {"1", "fin B1 got1 D1"}});
}

// Sync control (§28.3): the non-async return edge already keeps the returned
// local out of its disposals. These two shapes are exactly-once there too.
BEZEL_TEST(async_multi_return_owned_local_6052,
          sync_control_fresh_value_in_if_local_returned_last_28_3) {
    expectExactlyOnce(
        "owned Dq", "c: bool",
        "var o := new Dq(1); "
        "if c then return new Dq(2); end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "got1 D1"}},
        false, "%s.id.ToString()", /*async=*/false);
}

BEZEL_TEST(async_multi_return_owned_local_6052,
          sync_control_inner_local_then_outer_local_28_3) {
    expectExactlyOnce(
        "owned Dq", "c: bool",
        "var o := new Dq(1); "
        "if c then var q := new Dq(2); return q; end if; "
        "return o; ",
        {{"true", "D1 got2 D2"}, {"false", "got1 D1"}},
        false, "%s.id.ToString()", /*async=*/false);
}
