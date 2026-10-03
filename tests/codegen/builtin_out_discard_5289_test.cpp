// Regression tests for #5289 (a recurrence of closed #1315) — an `out _`
// discard passed to a BUILTIN `Try*` intrinsic made the call yield false.
//
// WHAT THE DOCUMENTS REQUIRE.  Spec §17.13 (v0.313): "`out _` does not
// introduce a variable; the receiving slot is internal to the compiler and is
// not observable. The argument is type-inferred from the parameter's declared
// type." §22.6: "against `out T` the received value is dropped; against `out
// owned T` the discarded value is an unbound owned temporary disposed at the
// end of the full statement (§28.3)". So `int.TryParse("5", out _)` is exactly
// as true as `int.TryParse("5", out var n)`; the discard changes nothing about
// the call's value. The stdlib out shapes are §2.2 (Math.DivRem), §2.3
// (Convert.TryTo*), §2.5/§2.6 (DateTime/DateOnly/TimeOnly/Guid.TryParse), §4
// (collection Try*), §4.14/§4.15 (TryRead*), §6.9 (Channel.TryReceive), §2.16
// (Weak.TryUpgrade) and §9.1-§9.5 (primitive TryParse).
//
// MEASURED PRE-FIX (staging r23-c28 1185f876): every builtin Try* below printed
// `no` / `False` (or, for the WriteLine-argument form, emitted nothing at all
// behind a #3079 `internal:` note) because each builtin site guarded on
// `call.args[N].value`, which is null BY DESIGN for a discard (Node.hpp
// `CallArgument::value`: "null when isDiscard"); the only builtin that worked
// was `Dictionary.TryGetValue` (#1301's own special case). #1315 had declared
// this follow-up and then closed without a code change at these sites.
//
// Every row asserts stdout byte-for-byte AND exit 0 (the `var b := ...` form
// also pins that #2434's "degraded to a zero-initialized value" warning's
// cause is gone: the printed value is the real one). Each discard row has a
// non-discard twin (`out var x`) printing the identical text.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string main_(const std::string& body) {
    return std::string("module M; import System; class C "
                       "public static method Main(): int ") +
           body + " return 0; end Main; end C;";
}

void expectOut(const std::string& body, const std::string& want) {
    auto r = runAot(main_(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// `if E then ok else no; var b := E; WriteLine(b)` for a call text E.
std::string ifAndVar(const std::string& e) {
    return "if " + e + " then Console.WriteLine(\"ok\"); "
           "else Console.WriteLine(\"no\"); end; "
           "var b := " + e + "; Console.WriteLine(b); ";
}

// Discard row + twin for one builtin. `callFmt` has `@` where the out argument
// goes; `pre` is any setup statements. Success input => "ok\nTrue\n".
void expectDiscardAndTwin(const std::string& pre, const std::string& callFmt,
                          const std::string& want) {
    auto withArg = [&](const std::string& arg) {
        std::string e = callFmt;
        e.replace(e.find('@'), 1, arg);
        return e;
    };
    expectOut(pre + ifAndVar(withArg("out _")), want);
    // Twin: a real named recipient. `if` and `var` each need their own
    // recipient (a second `out var d` in the same scope would redeclare), so
    // the twin uses two names.
    std::string e1 = callFmt, e2 = callFmt;
    e1.replace(e1.find('@'), 1, "out var d1");
    e2.replace(e2.find('@'), 1, "out var d2");
    expectOut(pre + "if " + e1 + " then Console.WriteLine(\"ok\"); "
                    "else Console.WriteLine(\"no\"); end; "
                    "var b := " + e2 + "; Console.WriteLine(b); ",
              want);
}

}  // namespace

// ---- §9.1-§9.5 primitive TryParse tower ------------------------------------

BEZEL_TEST(builtin_out_discard_5289, int_tryparse_spec_17_13_22_6) {
    expectDiscardAndTwin("", "int.TryParse(\"5\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, int_tryparse_failure_input) {
    // The discard must not make the call constant-true either.
    expectOut(ifAndVar("int.TryParse(\"x\", out _)"), "no\nFalse\n");
}
BEZEL_TEST(builtin_out_discard_5289, long_tryparse) {
    expectDiscardAndTwin("", "long.TryParse(\"9876543210\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, uint_tryparse) {
    expectDiscardAndTwin("", "uint.TryParse(\"7\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, double_tryparse) {
    expectDiscardAndTwin("", "double.TryParse(\"5.5\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, float_tryparse) {
    expectDiscardAndTwin("", "float.TryParse(\"5.5\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, decimal_tryparse) {
    expectDiscardAndTwin("", "decimal.TryParse(\"5.5\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, decimal_tryparse_failure_input) {
    expectOut(ifAndVar("decimal.TryParse(\"zz\", out _)"), "no\nFalse\n");
}
BEZEL_TEST(builtin_out_discard_5289, bool_tryparse) {
    expectDiscardAndTwin("", "bool.TryParse(\"true\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, char_tryparse) {
    expectDiscardAndTwin("", "char.TryParse(\"c\", @)", "ok\nTrue\n");
}

// ---- §2.3 Convert.TryTo* ---------------------------------------------------

BEZEL_TEST(builtin_out_discard_5289, convert_trytoint32) {
    expectDiscardAndTwin("", "Convert.TryToInt32(\"5\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, convert_trytoint64) {
    expectDiscardAndTwin("", "Convert.TryToInt64(\"5\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, convert_trytodouble) {
    expectDiscardAndTwin("", "Convert.TryToDouble(\"5\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, convert_trytodouble_failure_input) {
    expectOut(ifAndVar("Convert.TryToDouble(\"x\", out _)"), "no\nFalse\n");
}
BEZEL_TEST(builtin_out_discard_5289, convert_trytodecimal) {
    expectDiscardAndTwin("", "Convert.TryToDecimal(\"5\", @)", "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, convert_trytoboolean) {
    expectDiscardAndTwin("", "Convert.TryToBoolean(\"true\", @)", "ok\nTrue\n");
}

// ---- §2.5 / §2.6 DateTime, DateOnly, TimeOnly, Guid ------------------------

BEZEL_TEST(builtin_out_discard_5289, datetime_tryparse) {
    expectDiscardAndTwin("", "DateTime.TryParse(\"2024-01-01\", @)",
                         "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, dateonly_tryparse) {
    expectDiscardAndTwin("", "DateOnly.TryParse(\"2024-01-01\", @)",
                         "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, timeonly_tryparse) {
    expectDiscardAndTwin("", "TimeOnly.TryParse(\"10:00:00\", @)",
                         "ok\nTrue\n");
}
BEZEL_TEST(builtin_out_discard_5289, guid_tryparse) {
    expectDiscardAndTwin(
        "", "Guid.TryParse(\"00000000-0000-0000-0000-000000000001\", @)",
        "ok\nTrue\n");
}

// ---- §2.2 Math.DivRem ------------------------------------------------------

BEZEL_TEST(builtin_out_discard_5289, math_divrem_discard_remainder) {
    // The quotient is the call's value; the remainder is dropped.
    expectOut("var q := Math.DivRem(17, 5, out _); Console.WriteLine(q); "
              "var r := Math.DivRem(17, 5, out var rem); "
              "Console.WriteLine(r); Console.WriteLine(rem);",
              "3\n3\n2\n");
}

// ---- §4 collection Try* (borrowing) ----------------------------------------

BEZEL_TEST(builtin_out_discard_5289, stack_trypop_and_empty) {
    expectOut("var s := new Stack<int>(); s.Push(7); "
              "Console.WriteLine(s.TryPop(out _)); "
              "Console.WriteLine(s.TryPop(out _)); "
              "var t := new Stack<int>(); t.Push(8); "
              "Console.WriteLine(t.TryPop(out var x)); Console.WriteLine(x);",
              "True\nFalse\nTrue\n8\n");
}
BEZEL_TEST(builtin_out_discard_5289, queue_trydequeue_and_trypeek) {
    expectOut("var q := new Queue<int>(); q.Enqueue(7); "
              "Console.WriteLine(q.TryPeek(out _)); "
              "Console.WriteLine(q.TryDequeue(out _)); "
              "Console.WriteLine(q.TryDequeue(out _));",
              "True\nTrue\nFalse\n");
}
BEZEL_TEST(builtin_out_discard_5289, stack_trypop_class_elements_if_and_var) {
    expectOut("var s := new Stack<string>(); s.Push(\"a\"); "
              "if s.TryPop(out _) then Console.WriteLine(\"ok\"); "
              "else Console.WriteLine(\"no\"); end; "
              "var b := s.TryPop(out _); Console.WriteLine(b);",
              "ok\nFalse\n");
}
BEZEL_TEST(builtin_out_discard_5289, dictionary_trygetvalue_control_1301) {
    // The one builtin that already worked (#1301): stays working.
    expectOut("var d := new Dictionary<string, int>(); d.Add(\"a\", 1); "
              "Console.WriteLine(d.TryGetValue(\"a\", out _)); "
              "Console.WriteLine(d.TryGetValue(\"z\", out _));",
              "True\nFalse\n");
}

// ---- §22.6 `out _` against `out owned`: dropped value is disposed ----------

namespace {
const char* kR =
    "class R public static count: int := 0; "
    "  destructor R.count := R.count + 1; end destructor; end R; ";
std::string withR(const std::string& body) {
    return std::string("module M; import System; ") + kR +
           "class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}
void expectROut(const std::string& body, const std::string& want) {
    auto r = runAot(withR(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}
}  // namespace

BEZEL_TEST(builtin_out_discard_5289, owningarray_trypopfirst_disposes_discard) {
    // 2 elements popped+discarded => 2 destructors by the time we print; the
    // 3rd call is on an empty array (false, nothing to dispose).
    expectROut("do using var a := new OwningArray<R>(); "
               "  a.Add(move (new R())); a.Add(move (new R())); "
               "  Console.WriteLine(a.TryPopFirst(out _)); "
               "  Console.WriteLine(a.TryPopLast(out _)); "
               "  Console.WriteLine(a.TryPopLast(out _)); "
               "  Console.WriteLine(R.count); end;",
               "True\nTrue\nFalse\n2\n");
}
BEZEL_TEST(builtin_out_discard_5289, owninglinkedlist_trypopfirst_disposes_discard) {
    expectROut("do using var l := new OwningLinkedList<R>(); "
               "  l.AddLast(move (new R())); "
               "  Console.WriteLine(l.TryPopFirst(out _)); "
               "  Console.WriteLine(l.TryPopFirst(out _)); "
               "  Console.WriteLine(R.count); end;",
               "True\nFalse\n1\n");
}
BEZEL_TEST(builtin_out_discard_5289, owningdictionary_trypopvalue_disposes_discard) {
    expectROut("do using var d := new OwningDictionary<string, R>(); "
               "  d.Add(\"k\", move (new R())); "
               "  Console.WriteLine(d.TryPopValue(\"k\", out _)); "
               "  Console.WriteLine(d.TryPopValue(\"k\", out _)); "
               "  Console.WriteLine(R.count); end;",
               "True\nFalse\n1\n");
}
BEZEL_TEST(builtin_out_discard_5289, owningarray_trypopfirst_discard_in_loop) {
    // Loop shape: every iteration's discarded element is disposed (500 total);
    // a leak or a double free shows in the count / under the ASan leg.
    expectROut("do using var a := new OwningArray<R>(); "
               "  for var i in 0..499 do a.Add(move (new R())); end; "
               "  var n := 0; "
               "  while a.TryPopFirst(out _) do n := n + 1; end; "
               "  Console.WriteLine(n); Console.WriteLine(R.count); end;",
               "500\n500\n");
}

// ---- §4.14/§4.15 TryRead*, §6.9 TryReceive, §2.16 TryUpgrade ---------------

BEZEL_TEST(builtin_out_discard_5289, span_tryread_discard) {
    expectOut("using var b := new Array<byte>(8, 0); "
              "Console.WriteLine(b.AsReadOnlySpan().TryReadInt32LittleEndian(out _)); "
              "Console.WriteLine(b.AsReadOnlySpan().TryReadInt64BigEndian(out _)); "
              "using var s := new Array<byte>(3, 0); "
              "Console.WriteLine(s.AsReadOnlySpan().TryReadInt32LittleEndian(out _));",
              "True\nTrue\nFalse\n");
}
BEZEL_TEST(builtin_out_discard_5289, channel_tryreceive_discard) {
    expectOut("using var ch := new Channel<int>(); "
              "Console.WriteLine(ch.TryReceive(out _));",
              "False\n");
}

BEZEL_TEST(builtin_out_discard_5289, weak_tryupgrade_discard_releases_handle) {
    // `out _` against `out using var s: Shared<T>`: the upgraded handle is an
    // unbound owned temporary (§22.6, §28.3) and must be RELEASED, else the
    // Cell outlives its last owner (count stays 1) or leaks under ASan.
    auto r = runAot(
        "module M; import System; "
        "class Cell public static _n: int := 0; "
        "  public constructor() Cell._n := Cell._n + 1; end constructor; "
        "  destructor Cell._n := Cell._n - 1; end destructor; end Cell; "
        "class C public static method Main(): int "
        "  do using var sh := new Shared<Cell>(new Cell()); "
        "    using var w := sh.Downgrade(); "
        "    if w.TryUpgrade(out _) then Console.WriteLine(\"ok\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    var b := w.TryUpgrade(out _); Console.WriteLine(b); "
        "  end; "
        "  Console.WriteLine(Cell._n); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\nTrue\n0\n");
}

// ---- lambda body and async body -------------------------------------------

BEZEL_TEST(builtin_out_discard_5289, discard_inside_lambda_body) {
    expectOut("var f := lambda(s: string): bool => int.TryParse(s, out _); "
              "Console.WriteLine(f(\"5\")); Console.WriteLine(f(\"x\")); "
              "var g := lambda(s: string): bool => do "
              "  if Convert.TryToDouble(s, out _) then return true; end; "
              "  return false; end; "
              "Console.WriteLine(g(\"2.5\")); Console.WriteLine(g(\"q\"));",
              "True\nFalse\nTrue\nFalse\n");
}
BEZEL_TEST(builtin_out_discard_5289, discard_inside_async_body) {
    auto r = runAot(
        "module M; import System; class C "
        "  public static async method Run(s: string): Task<bool> "
        "    await Task.Delay(1); "
        "    var ok := int.TryParse(s, out _); "
        "    var ok2 := DateTime.TryParse(\"2024-01-01\", out _); "
        "    await Task.Delay(1); "
        "    return ok and ok2; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine(await Run(\"5\")); "
        "    Console.WriteLine(await Run(\"x\")); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// ---- #1301 control: a user method's `out _` ---------------------------------

BEZEL_TEST(builtin_out_discard_5289, user_method_out_discard_control_1301) {
    auto r = runAot(
        "module M; class C "
        "  public static method Try(s: string, out n: int): bool "
        "    n := 5; return s = \"5\"; "
        "  end Try; "
        "  public static method Main(): int "
        "    if C.Try(\"5\", out _) then Console.WriteLine(\"ok\"); "
        "    else Console.WriteLine(\"no\"); end; "
        "    var b := C.Try(\"5\", out _); Console.WriteLine(b); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\nTrue\n");
}
