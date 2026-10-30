// A `string` (or reference-free record / inline-array) FIELD read of an unbound
// owned temporary disposes the temporary (fix #7728).
//
// §28.3 *Unbound owned temporaries* (v0.350): an owned value that is "not bound
// to any name, not `move`d, and not returned ... but used only by *borrowing*
// operations within the expression — as the receiver of a further call, as a
// non-`owned` argument, as an operator operand, as a member or index access"
// "is disposed at the **end of the full expression** in which it is produced,
// after every use of it within that expression has completed". K8.1 (v0.350):
// "Unbound owned temporaries are disposed at their full expression's end, after
// their uses, in reverse production order -- on the normal edge as on the
// exception edge". §28.10 (v0.350): "`using var` accepts a value-type or
// runtime-managed right-hand side ... the disposal at scope end is a no-op ...
// and the only effect is the reassignability restriction".
//
// A `string` is runtime-managed and copyable (§28.9), so reading one out of a
// temporary is not a K8.4 borrow escape: the binding gets its own reference and
// the temporary is disposed at the boundary. Before the fix the `int`-field twin
// was clean and the `string` field leaked the WHOLE temporary: its destructor
// never ran.
//
// THE ORACLE. Every row is `runAotChecked` (generated-code ASan + LSan), loops
// 100 times, and reads the destructor count off stdout (exit codes truncate at
// 8 bits). A use-after-free of the string fails the row too: the string content
// is read after the temporary is gone, and the strings are heap strings built at
// run time, not literals.
//
// MEASURED on the base (1295324e), bezelc --sanitize=address, loop x100:
// `using var s := Make.MkH(i).S;` -> destructor count 0 and a LeakSanitizer
// report; `const s := ...`, `var s := ...`, `new H(1).S` and `keep.Clone().S`
// likewise; the `int` field twin -> count 100, clean.
//
// WHY THE TEMPORARY CANNOT BE DISPOSED INLINE: a `string` field read is a
// borrowed reference into the temporary. The fix takes the string's +1 at the
// read (so the read is a +1 string producer, like a call result), then queues
// the temporary for the full expression's END ("after their uses", K8.1), the
// way #4552 queues the receiver of a string-returning method.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// Declarations shared by the loop rows. `Res.Count` counts H's destructor runs
// (H owns a `Res`, so a leaked H also leaves its Res undisposed). The strings
// are built at run time (heap strings, not literals).
const char* kPrelude = R"(
module M;
public class Res
  public static Count: int := 0;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
public record Pt(X: int, Y: int);
public class H
  public owned C: Res := new Res();
  public N: int := 7;
  public S: string;
  public P: Pt := new Pt(3, 4);
  public A: [3]int := [5, 6, 7];
  public constructor(n: int)
    S := "heap-" + n.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
  end constructor;
  public method Clone(): owned H => new H(1);
end H;
public class Make
  public static method MkH(n: int): owned H return new H(n); end MkH;
end Make;
)";

// `body` runs 100 times inside Main; `t` accumulates, `keep` is a named H for
// the `.Clone()` receiver. Prints "<t> <Res.Count>": every H built in the loop
// is disposed, so the count is the number of temporaries (0 when they leak).
std::string loopProgram(const std::string& members, const std::string& body) {
    return std::string(kPrelude) + "public class Program\n" + members + R"(
  public static method Main(): int
    var t := 0;
    var keep := new H(0);
    for var i in 0..<100 do
      do
        )" + body + R"(
      end;
    end;
    Console.WriteLine($"{t} {Res.Count}");
    return 0;
  end Main;
end Program;
)";
}

// "heap-" (5) + digits + "-" (1) + 39 x: 46 characters for a one-digit n, 47
// for two digits. `new H(1)` / `keep.Clone()`: 100 x 46 = 4600. `Make.MkH(i)`:
// 10 one-digit + 90 two-digit values = 460 + 4230 = 4690.

void expectLoop(const std::string& members, const std::string& body,
                const std::string& expected) {
    bezel::test::JitResult r = runAotChecked(loopProgram(members, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

void expectRun(const std::string& program, const std::string& expected) {
    bezel::test::JitResult r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// The `async Main` twin of loopProgram, with the async helpers the
// suspension rows use. `Dly` really suspends (Task.Delay), so the code after
// each `await` runs in a later `$Resume` activation (§30.13, v0.350).
std::string asyncLoopProgram(const std::string& body) {
    return std::string(kPrelude) + R"(public class Program
  public static async method Dly(i: int): Task<int> await Task.Delay(1); return 1; end Dly;
  public static async method Dly2(s: string): Task<int> await Task.Delay(1); return s.Length; end Dly2;
  public static method TP(p: Pt, k: int): int return p.X + k; end TP;
  public static method TA(a: [3]int, k: int): int return a[0] + k; end TA;
  public static method Two(a: string, b: int): int return a.Length + b; end Two;
  public static async method Body(i: int): Task<int> return Make.MkH(i).S.Length + await Dly(i); end Body;
  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0..<100 do
      )" + body + R"(
    end;
    Console.WriteLine($"{t} {Res.Count}");
    return 0;
  end Main;
end Program;
)";
}

// Iterator methods for the deferred-frame rows. Each yields three values per
// call, read when the loop pulls them, after the call's full expression ended.
const char* kIterMembers = R"(
  public static method Gen(s: string): IEnumerable<int> for var k in 0..<3 do yield return s.Length + k; end; end Gen;
  public static method GenP(p: Pt): IEnumerable<int> for var k in 0..<3 do yield return p.X + k; end; end GenP;
  public static method GenA(a: [3]int): IEnumerable<int> for var k in 0..<3 do yield return a[k]; end; end GenA;
  public static method Id(s: string): string return s; end Id;
  public static method None(): string? return null; end None;
  public static method En(s: string): IEnumerator<int> return Gen(s).GetEnumerator(); end En;
  public static async method AIt(s: string): IAsyncEnumerable<int> for var k in 0..<3 do await Task.Delay(0); yield return s.Length + k; end; end AIt;
  public static async method AGen(s: string): Task<IEnumerable<int>> return Gen(s); end AGen;
)";

// The `async Main` twin of loopProgram over kIterMembers, for the
// async-iterator and Task<IEnumerable> rows.
std::string asyncIterProgram(const std::string& body) {
    return std::string(kPrelude) + "public class Program\n" + kIterMembers + R"(
  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0..<100 do
      )" + body + R"(
    end;
    Console.WriteLine($"{t} {Res.Count}");
    return 0;
  end Main;
end Program;
)";
}

// `var it := await AGen(...)` (an `async` method returning Task<IEnumerable>)
// leaks the iterator FRAME whatever the argument is. The `int` twin leaks the
// same frame on the base, which is a separate pre-existing defect and not part
// of #7728. So LeakSanitizer is off for this one helper (runAotCheckedLenient;
// generated-code ASan stays on). The printed Res count still pins the #7728
// reclaim: every temporary is disposed.
void expectAsyncIterLenient(const std::string& body, const std::string& expected) {
    bezel::test::JitResult r = runAotCheckedLenient(asyncIterProgram(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// fix #7728 (§28.3, K8.1, §28.10, v0.350): the issue's `using var` shape over a
// `new` receiver.
BEZEL_TEST(fresh_temp_field_read, using_var_string_field_of_new_temporary_spec_28_3_7728) {
    expectLoop("", "using var s := new H(1).S; t := t + s.Length;", "4600 100\n");
}

// The issue's repro shape: an `owned`-returning call receiver.
BEZEL_TEST(fresh_temp_field_read, using_var_string_field_of_call_temporary_spec_28_3_7728) {
    expectLoop("", "using var s := Make.MkH(i).S; t := t + s.Length;", "4690 100\n");
}

// An `owned`-returning method on a named holder.
BEZEL_TEST(fresh_temp_field_read, using_var_string_field_of_method_temporary_spec_28_3_7728) {
    expectLoop("", "using var s := keep.Clone().S; t := t + s.Length;", "4600 100\n");
}

// The issue's second shape: `const`.
BEZEL_TEST(fresh_temp_field_read, const_string_field_of_temporary_spec_28_3_7728) {
    expectLoop("", "const s := Make.MkH(i).S; t := t + s.Length;", "4690 100\n");
}

// The plain `var` twin (issues #7402 / #7445 / #7627 name this family): a string
// binding is a copy, so it leaked the temporary the same way.
BEZEL_TEST(fresh_temp_field_read, var_string_field_of_temporary_spec_28_3_7728) {
    expectLoop("", "var s := Make.MkH(i).S; t := t + s.Length;", "4690 100\n");
}

// The control: the `int` field of the same temporary was always clean.
BEZEL_TEST(fresh_temp_field_read, int_field_of_temporary_is_clean_control_spec_28_3_7728) {
    expectLoop("", "using var s := Make.MkH(i).N; t := t + s;", "700 100\n");
}

// The consumers of a +1 string producer: a `.Length` receiver, a `+` operand, an
// interpolation hole, a plain argument, a comparison, an if-expression arm. Each
// reads the string after the field read and before the temporary's disposal, and
// releases its own reference exactly once (a use-after-free or a leak fails).
// Per iteration: 46 + 47 + 48 + 46 + 1 (the comparison) = 188, plus 46 on an even
// `i` (the if-expression's temporary arm) or 5 ("short") on an odd one:
// 100 x 188 + 50 x 46 + 50 x 5 = 21350. H built per iteration: 6, plus 1 on even
// `i`: 650.
BEZEL_TEST(fresh_temp_field_read, string_field_consumers_spec_28_3_7728) {
    expectLoop(
        "  public static method Take(s: string): int return s.Length; end Take;",
        "t := t + Make.MkH(1).S.Length; "
        "t := t + (Make.MkH(2).S + \"k\").Length; "
        "t := t + $\"<{Make.MkH(3).S}>\".Length; "
        "t := t + Take(Make.MkH(4).S); "
        "if Make.MkH(5).S = Make.MkH(5).S then t := t + 1; end; "
        "var c := (if i mod 2 = 0 then Make.MkH(6).S else \"short\"); "
        "t := t + c.Length;",
        "21350 650\n");
}

// The string's content is intact AFTER the temporary is gone (a use-after-free
// would print garbage or trip ASan).
BEZEL_TEST(fresh_temp_field_read, string_content_survives_temporary_disposal_spec_28_3_7728) {
    std::string prog = std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var last := "";
    for var i in 0..<100 do
      var s := Make.MkH(i).S;
      last := s;
    end;
    Console.WriteLine($"{last} {Res.Count}");
    return 0;
  end Main;
end Program;
)";
    bezel::test::JitResult r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "heap-99-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx 100\n");
}

// K8.1: the temporary is disposed at its full expression's END, "after their
// uses", not inline at the field read. `Tag()` runs AFTER the field read and
// prints before the statement's `WriteLine`; the destructor's "D" follows the
// whole statement's output and precedes the next statement.
BEZEL_TEST(fresh_temp_field_read, temporary_is_disposed_at_full_expression_end_spec_28_3_7728) {
    bezel::test::JitResult r = runAotChecked(R"(
module M;
public class HD
  public S: string;
  public constructor(n: int)
    S := "heap-" + n.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
  end constructor;
  destructor Console.WriteLine("D"); end destructor;
end HD;
public class Program
  public static method Tag(): string
    Console.WriteLine("tag");
    return "!";
  end Tag;
  public static method Mk(n: int): owned HD return new HD(n); end Mk;
  public static method Main(): int
    Console.WriteLine(Mk(1).S + Tag());
    Console.WriteLine("after");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "tag\nheap-1-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx!\nD\nafter\n");
}

// The same temporary leak for a reference-free INLINE-ARRAY field (a plain data
// copy, so no retain is owed): 100 x (5 + 6 + 7) = 1800.
BEZEL_TEST(fresh_temp_field_read, inline_array_field_of_temporary_spec_28_3_7728) {
    expectLoop("", "var a := Make.MkH(i).A; t := t + a[0] + a[1] + a[2];", "1800 100\n");
}

// ... and for a reference-free RECORD field (`Pt` holds two ints): 100 x 7.
BEZEL_TEST(fresh_temp_field_read, int_record_field_of_temporary_spec_28_3_7728) {
    expectLoop("", "var p := Make.MkH(i).P; t := t + p.X + p.Y;", "700 100\n");
}

BEZEL_TEST(fresh_temp_field_read, const_int_record_field_of_new_temporary_spec_28_3_7728) {
    expectLoop("", "const p := new H(1).P; t := t + p.X + p.Y;", "700 100\n");
}

// --- A suspension in the same full expression (review of #7728) -------------
//
// The receiver is queued for the full expression's end; when an `await` in
// that expression suspends, the flush runs in a later `$Resume` activation and
// must find the receiver in the frame (fix #7799's spill, which the queue
// shares). Measured on the first #7728 commit (07bc2343) without that spill:
// each of these rows was a heap-use-after-free or a SEGV with no output; on
// the base they leaked every temporary (count 0). Target: no sanitizer report.

// The field read before the suspension: 4690 + 100 x 1.
BEZEL_TEST(fresh_temp_field_read, string_field_then_await_same_expression_spec_28_3_7728) {
    expectRun(asyncLoopProgram("t := t + Make.MkH(i).S.Length + await Dly(i);"),
              "4790 100\n");
}

// The field read is the awaited call's argument.
BEZEL_TEST(fresh_temp_field_read, string_field_argument_of_awaited_call_spec_28_3_7728) {
    expectRun(asyncLoopProgram("t := t + await Dly2(Make.MkH(i).S);"), "4690 100\n");
}

// The field read is an argument of a call whose LATER argument suspends.
BEZEL_TEST(fresh_temp_field_read, string_field_argument_then_await_argument_spec_28_3_7728) {
    expectRun(asyncLoopProgram("t := t + Two(Make.MkH(i).S, await Dly(i));"),
              "4790 100\n");
}

// The same shape in a non-Main async method's `return`.
BEZEL_TEST(fresh_temp_field_read, string_field_then_await_in_async_method_spec_28_3_7728) {
    expectRun(asyncLoopProgram("t := t + await Body(i);"), "4790 100\n");
}

// The data-copy branch: a record field (p.X = 3) and an inline-array field
// (a[0] = 5) as an argument before an awaited argument.
BEZEL_TEST(fresh_temp_field_read, record_field_argument_then_await_spec_28_3_7728) {
    expectRun(asyncLoopProgram("t := t + TP(Make.MkH(i).P, await Dly(i));"),
              "400 100\n");
}

BEZEL_TEST(fresh_temp_field_read, inline_array_field_argument_then_await_spec_28_3_7728) {
    expectRun(asyncLoopProgram("t := t + TA(Make.MkH(i).A, await Dly(i));"),
              "600 100\n");
}

// --- An iterator's argument (review of #7728) -------------------------------
//
// A call whose result is an iterable reads its arguments after the call's full
// expression has ended. The iterator frame holds its own reference to a string
// it captures (fix #7798), so the +1 the field read took is released at the
// call's end and the temporary is reclaimed at the K8 boundary like any other.
// Measured on the first #7728 commit (07bc2343) and on its rejected
// fix-forward (84556669, which withheld the reclaim only for a DIRECT argument
// of such a call): a heap-use-after-free in MoveNext for the direct argument
// and, on 84556669, for every indirect one below (an if-expr or `case` arm,
// `??`, a call wrapping the read, `await for`, Task<IEnumerable>, an
// IEnumerator built from it). On the base they all leaked every temporary
// (count 0). 46/47 + k over k = 0..2, x100: 3 x 4690 + 300 = 14370. With a
// "lit" arm for odd i: 5 x 141 + 45 x 144 + 50 x 12 = 7785 and 50 temporaries.

BEZEL_TEST(fresh_temp_field_read, string_field_argument_of_iterator_in_for_spec_28_3_7728) {
    expectLoop(kIterMembers, "for var x in Gen(Make.MkH(i).S) do t := t + x; end;",
               "14370 100\n");
}

BEZEL_TEST(fresh_temp_field_read, string_field_argument_of_bound_iterator_spec_28_3_7728) {
    expectLoop(kIterMembers, "var it := Gen(Make.MkH(i).S); for var x in it do t := t + x; end;",
               "14370 100\n");
}

// The read in an if-expression arm (bound, and inline in the `for`).
BEZEL_TEST(fresh_temp_field_read, string_field_in_ifexpr_argument_of_iterator_spec_28_3_7728) {
    expectLoop(kIterMembers,
               "var it := Gen((if i mod 2 = 0 then Make.MkH(i).S else \"lit\")); "
               "for var x in it do t := t + x; end;",
               "7785 50\n");
    expectLoop(kIterMembers,
               "for var x in Gen((if i mod 2 = 0 then Make.MkH(i).S else \"lit\")) do t := t + x; end;",
               "7785 50\n");
}

// The read in a `case` arm.
BEZEL_TEST(fresh_temp_field_read, string_field_in_case_argument_of_iterator_spec_28_3_7728) {
    expectLoop(kIterMembers,
               "var it := Gen((case i mod 2 of 0 => Make.MkH(i).S, _ => \"lit\")); "
               "for var x in it do t := t + x; end;",
               "7785 50\n");
}

// The read as the right operand of `??`.
BEZEL_TEST(fresh_temp_field_read, string_field_in_coalesce_argument_of_iterator_spec_28_3_7728) {
    expectLoop(kIterMembers,
               "var it := Gen(None() ?? Make.MkH(i).S); for var x in it do t := t + x; end;",
               "14370 100\n");
}

// The read wrapped in another call (`Id` returns its borrowed parameter).
BEZEL_TEST(fresh_temp_field_read, string_field_through_call_into_iterator_spec_28_3_7728) {
    expectLoop(kIterMembers,
               "var it := Gen(Id(Make.MkH(i).S)); for var x in it do t := t + x; end;",
               "14370 100\n");
}

// Reads from two different temporaries in the two arms (100 temporaries).
// keep.Clone().S is 46 characters: 50 x 141 + 5 x 141 + 45 x 144 = 14235.
BEZEL_TEST(fresh_temp_field_read, string_fields_in_both_ifexpr_arms_of_iterator_spec_28_3_7728) {
    expectLoop(kIterMembers,
               "var it := Gen((if i mod 2 = 0 then Make.MkH(i).S else keep.Clone().S)); "
               "for var x in it do t := t + x; end;",
               "14235 100\n");
}

// An IEnumerator built from the iterator inside the callee
// (`Gen(s).GetEnumerator()`) and driven by hand.
BEZEL_TEST(fresh_temp_field_read, string_field_argument_of_enumerator_helper_spec_28_3_7728) {
    expectLoop(kIterMembers,
               "using var e := En(Make.MkH(i).S); while e.MoveNext() do t := t + e.Current; end;",
               "14370 100\n");
}

// The async iterator, `await for` over an if-expression argument and over the
// direct argument.
BEZEL_TEST(fresh_temp_field_read, string_field_argument_of_async_iterator_spec_28_3_7728) {
    expectRun(asyncIterProgram(
                  "await for var x in AIt((if i mod 2 = 0 then Make.MkH(i).S else \"lit\")) do t := t + x; end;"),
              "7785 50\n");
    expectRun(asyncIterProgram("await for var x in AIt(Make.MkH(i).S) do t := t + x; end;"),
              "14370 100\n");
}

// The read as the argument of an `async` method that returns the iterator
// (Task<IEnumerable>): LeakSanitizer off, see expectAsyncIterLenient.
BEZEL_TEST(fresh_temp_field_read, string_field_argument_of_task_of_iterator_spec_28_3_7728) {
    expectAsyncIterLenient("var it := await AGen(Make.MkH(i).S); for var x in it do t := t + x; end;",
                           "14370 100\n");
}

// A reference-free record or inline-array argument is COPIED into the frame,
// so its temporary is reclaimed (strict oracle: no leak): 100 x (3+4+5) = 1200
// and 100 x (5+6+7) = 1800.
BEZEL_TEST(fresh_temp_field_read, record_field_argument_of_bound_iterator_spec_28_3_7728) {
    expectLoop(kIterMembers, "var it := GenP(Make.MkH(i).P); for var x in it do t := t + x; end;",
               "1200 100\n");
}

BEZEL_TEST(fresh_temp_field_read, inline_array_field_argument_of_bound_iterator_spec_28_3_7728) {
    expectLoop(kIterMembers, "var it := GenA(Make.MkH(i).A); for var x in it do t := t + x; end;",
               "1800 100\n");
}
