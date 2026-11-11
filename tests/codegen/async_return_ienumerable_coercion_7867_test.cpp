// The §29.2 coercion on the ASYNC `return` edge (fix #7867).
//
// §30.1 (v0.357): an `async` method returns "`Task<T>` for some type T ... — a
// result of type T", and its `return expression;` completes the task with that
// result. For `Task<IEnumerable<T>>` the result is an `IEnumerable<T>`, the
// type §29.2 (v0.357) desugars a `for` over as
// `using var __enumerator := __source.GetEnumerator();`. stdlib §5 (v0.357)
// declares `Where(predicate: (T) -> bool): IEnumerable<T>`, so
// `return s.Where(f);` in such a method is a well-typed result.
//
// THE DEFECT. The sync `return` edge converts the returned value to the
// declared `IEnumerable<T>` representation (a lazy LINQ pipeline is drained
// into an owning enumerator, a collection is wrapped). The async edge
// completed the task and returned BEFORE that conversion, so the task carried
// the raw pipeline handle (or the raw collection), and the awaiter's `for` /
// `ToArray()` called `GetEnumerator` through a slot the value does not have.
//
// MEASURED on the base (71317e6c9), bezelc --sanitize=address, loop x200:
// `var it := await AW(x); for var c in it do ... end;` with
// `AW(s): Task<IEnumerable<char>> return s.Where(lambda(c) => true);` is
// `AddressSanitizer: SEGV on unknown address` (pc 0x2) for EVERY argument kind
// — the issue's field read of a fresh temporary, a named holder's field, a
// local, a literal, a concat, a call result and a forwarded parameter — and for
// `Select` as for `Where`. The non-async twin of every row is clean. So the
// argument is not the cause; the issue's field read only made it visible.
//
// The same edge carried every other source family: an `Array`, `Stack` or
// `Set` the body built, an `Array` parameter, a lazy pipeline over one — all
// SEGV on the base, all clean in the non-async twin.
//
// THE ORACLE. Every row is `runAotChecked` (generated-code ASan + LSan) unless
// it says otherwise, loops 200 times, and reads its counts off stdout (exit
// codes truncate at 8 bits). The strings are heap strings built at run time,
// so a read of a freed argument fails the row. `Res.Count` counts the
// destructor runs of the `H` temporaries the field-read rows build.
//
// WHY THE STRICT ROWS CONSUME THE RESULT INSIDE THE AWAIT'S OWN EXPRESSION
// (`(await AW(x)).Count()`, `(await AW(x)).ToArray()`). A `Task<IEnumerable<T>>`
// result that is BOUND (`var it := await AW(x);`) is disposed by nobody — not
// by the task, not by the awaiter. That is #8528, pre-existing and independent
// of this fix: MEASURED on the base, the iterator twin
// `AGen(s): Task<IEnumerable<int>> return Gen(s);` already leaks its frame on
// that spelling (pinned LeakSanitizer-off by the #7728 suite). So the bound
// rows — the issue's literal repro among them — run with LeakSanitizer OFF
// (`runAotCheckedLenient`; generated-code ASan stays on, so the SEGV this fix
// removes still fails them), and say so. They move to the strict oracle with
// #8528.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

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
end H;
public class Make
  public static method MkH(n: int): owned H return new H(n); end MkH;
end Make;
)";

// The async producers under test, a forwarding caller (a TRUE parameter
// argument), and the non-async twins (the control). `Where` drops the 'x's, so
// a row that enumerated the wrong thing prints a different number.
const char* kMembers = R"(
  public static async method AW(s: string): Task<IEnumerable<char>> return s.Where(lambda(c) => c <> 'x'); end AW;
  public static async method AS(s: string): Task<IEnumerable<char>> return s.Select(lambda(c) => c); end AS;
  public static async method Fwd(p: string): Task<int> return (await AW(p)).Count(); end Fwd;
  public static method NW(s: string): IEnumerable<char> return s.Where(lambda(c) => c <> 'x'); end NW;
  public static method Id(s: string): string return s; end Id;
)";

// `body` runs 200 times inside an async Main. `keep` is a named holder, `loc`
// a heap string local, `shared` an Array the collection rows pass. Prints
// "<t> <Res.Count>".
std::string program(const std::string& members, const std::string& body) {
    return std::string(kPrelude) + "public class Program\n" + members + R"(
  public static async method Main(): Task<int>
    var t := 0;
    using var keep := new H(0);
    var loc := "local-" + t.ToString() + "-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";
    var shared: Array<int> := [1, 2, 3];
    for var i in 0..<200 do
      )" + body + R"(
    end;
    Console.WriteLine($"{t} {Res.Count}");
    return 0;
  end Main;
end Program;
)";
}

void expectStrict(const std::string& members, const std::string& body,
                  const std::string& expected) {
    bezel::test::JitResult r = runAotChecked(program(members, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// LeakSanitizer off: the BOUND result of a `Task<IEnumerable<T>>` has no owner
// (#8528, see the file header). Generated-code ASan is on.
void expectBoundLenient(const std::string& members, const std::string& body,
                        const std::string& expected) {
    bezel::test::JitResult r = runAotCheckedLenient(program(members, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// One async producer `AW` with the given body, consumed in the await's own
// expression. The argument is the issue's: a field read of a fresh temporary.
void expectShape(const std::string& awBody, const std::string& expected) {
    expectStrict("  public static async method AW(s: string): Task<IEnumerable<char>> " +
                     awBody + "\n",
                 "t := t + (await AW(Make.MkH(i).S)).Count();", expected);
}

// A producer `AF(s, xs): Task<IEnumerable<int>>` with the given body, summed
// in the await's own expression.
void expectFamily(const std::string& afBody, const std::string& expected) {
    expectStrict("  public static async method AF(s: string, xs: Array<int>): "
                 "Task<IEnumerable<int>> " + afBody + " end AF;\n",
                 "t := t + (await AF(Make.MkH(i).S, shared)).Sum();", expected);
}

// THE ISSUE'S OWN REPRO, verbatim
// (/var/tmp/bezel-scratch/r37-rev-u7728b/p2/q44_async_returns_where.bzl): its
// prelude, its `AW`, its loop of 100.
const char* kIssueRepro = R"(module M;
public class Res
  public static Count: int := 0;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
public record Pt(X: int, Y: int);
public record Rs(Name: string, N: int);
public class Inner
  public S: string;
  public owned C: Res := new Res();
  public constructor(n: int) S := "inner-" + n.ToString() + "-yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"; end constructor;
end Inner;
public class H
  public owned C: Res := new Res();
  public N: int := 7;
  public S: string;
  public Sn: string? := null;
  public owned In: Inner;
  public P: Pt := new Pt(3, 4);
  public R: Rs;
  public A: [3]int := [5, 6, 7];
  public constructor(n: int)
    S := "heap-" + n.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    In := new Inner(n);
    R := new Rs("lit", n);
  end constructor;
  public property Prop: string => S;
  public method Clone(): owned H => new H(1);
end H;
public class G<T>
  public owned C: Res := new Res();
  public V: T;
  public S: string;
  public constructor(v: T) V := v; S := "gen-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"; end constructor;
end G;
public class Holder
  public F: string := "";

end Holder;
public class Make
  public static method MkH(n: int): owned H return new H(n); end MkH;
  public static method MkHn(n: int): owned H? return new H(n); end MkHn;
  public static method MkG<T>(v: T): owned G<T> return new G<T>(v); end MkG;
  public static method Boom(n: int): int
    if n >= 0 then throw new InvalidOperationException("boom"); end;
    return 1;
  end Boom;
end Make;
public class Program
  public static async method AW(s: string): Task<IEnumerable<char>> return s.Where(lambda(c) => true); end AW;

  public static async method Main(): Task<int>
    var t := 0;
    using var keep := new H(0);
    using var holder := new Holder();

    for var i in 0..<100 do
      var it := await AW(Make.MkH(i).S); for var c in it do t := t + 1; end;
    end;

    Console.WriteLine($"{t} {Res.Count} {holder.F.Length}");
    return 0;
  end Main;
end Program;
)";

}  // namespace

// fix #7867 (§29.2, §30.1, v0.357): the issue's repro. Base: ASan SEGV, no
// output. 10 x 46 + 90 x 47 characters = 4690; each of the 100 `H` temporaries
// owns a `Res` and an `Inner` that owns one = 200 destructor runs (the #7728
// reclaim, intact); `holder.F` is "". The result is BOUND, so LeakSanitizer is
// off (#8528).
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           issue_repro_field_read_argument_where_bound_for_in_spec_29_2_30_1) {
    bezel::test::JitResult r = runAotCheckedLenient(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4690 200 0\n");
}

// The issue's producer and argument under the STRICT oracle (ASan + LSan): the
// result is counted inside the await's own expression. "heap-" + digits + "-"
// keeps 7 / 8 / 9 characters once the 39 'x' are dropped:
// 10 x 7 + 90 x 8 + 100 x 9 = 1690.
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           where_field_read_of_temporary_argument_count_spec_29_2_30_1) {
    expectStrict(kMembers, "t := t + (await AW(Make.MkH(i).S)).Count();", "1690 200\n");
}

// The argument-kind axis: the base crashed on every one, so none of them is
// the cause. A named holder's field (200 x 7), a local (200 x 46, no 'x'), a
// literal (200 x 6), a concat ("a" + digits + "xb" keeps 2 + digits:
// 10 x 3 + 90 x 4 + 100 x 5 = 890), a call result and a forwarded parameter
// (200 x 46).
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           where_every_other_argument_kind_count_spec_29_2_30_1) {
    expectStrict(kMembers, "t := t + (await AW(keep.S)).Count();", "1400 0\n");
    expectStrict(kMembers, "t := t + (await AW(loc)).Count();", "9200 0\n");
    expectStrict(kMembers, "t := t + (await AW(\"abcxdef\")).Count();", "1200 0\n");
    expectStrict(kMembers, "t := t + (await AW(\"a\" + i.ToString() + \"xb\")).Count();",
                 "890 0\n");
    expectStrict(kMembers, "t := t + (await AW(Program.Id(loc))).Count();", "9200 0\n");
    expectStrict(kMembers, "t := t + await Fwd(loc);", "9200 0\n");
}

// The `ToArray()` consumer: the materialised array is the caller's and is
// reclaimed; the awaited result is consumed in the same expression.
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           where_awaited_result_to_array_spec_29_2_30_1) {
    expectStrict(kMembers,
                 "var arr := (await AW(Make.MkH(i).S)).ToArray(); t := t + arr.Count;",
                 "1690 200\n");
    expectStrict(kMembers, "var arr := (await AW(loc)).ToArray(); t := t + arr.Count;",
                 "9200 0\n");
    expectStrict(kMembers,
                 "var arr := (await AW(\"a\" + i.ToString() + \"xb\")).ToArray(); "
                 "t := t + arr.Count;",
                 "890 0\n");
}

// The combinator axis: `Select` is the same edge. Full lengths:
// 10 x 46 + 90 x 47 + 100 x 48 = 9490.
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           select_awaited_result_count_and_to_array_spec_29_2_30_1) {
    expectStrict(kMembers, "t := t + (await AS(Make.MkH(i).S)).Count();", "9490 200\n");
    expectStrict(kMembers, "t := t + (await AS(loc)).Count();", "9200 0\n");
    expectStrict(kMembers,
                 "var arr := (await AS(Make.MkH(i).S)).ToArray(); t := t + arr.Count;",
                 "9490 200\n");
}

// The BOUND consumer (`await`, then `for`) over the other argument kinds and
// over `Select`: correct output, no crash. LeakSanitizer off (#8528).
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           bound_result_for_in_other_argument_kinds_spec_29_2_30_1) {
    expectBoundLenient(kMembers,
                       "var it := await AW(loc); for var c in it do t := t + 1; end;",
                       "9200 0\n");
    expectBoundLenient(kMembers,
                       "var it := await AW(\"a\" + i.ToString() + \"xb\"); "
                       "for var c in it do t := t + 1; end;",
                       "890 0\n");
    expectBoundLenient(kMembers,
                       "var it := await AS(Make.MkH(i).S); for var c in it do t := t + 1; end;",
                       "9490 200\n");
    expectBoundLenient(kMembers,
                       "var it := await AW(Make.MkH(i).S); var arr := it.ToArray(); "
                       "t := t + arr.Count;",
                       "1690 200\n");
}

// The result awaited and NEVER enumerated. This row is green on the base too
// (the base printed the same and leaked the raw pipeline: 1400 allocations
// over the 200 iterations; the drained result is 600), so it is a control for
// the mirror, not a pin of the fix. LeakSanitizer off (#8528).
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           bound_result_never_enumerated_control_spec_30_1) {
    expectBoundLenient(kMembers, "var it := await AW(Make.MkH(i).S); t := t + 1;",
                       "200 200\n");
}

// The return-edge shapes. A real suspension before the `return` (the value is
// produced in a later `$Resume` activation):
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           return_after_a_suspension_spec_29_2_30_1) {
    expectShape("await Task.Delay(0); return s.Where(lambda(c) => c <> 'x'); end AW;",
                "1690 200\n");
}

// A `return` routed through a `finally`: the coerced value is what is stashed.
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           return_through_finally_spec_29_2_30_1) {
    expectShape("try return s.Where(lambda(c) => c <> 'x'); "
                "finally Res.Count := Res.Count + 0; end; end AW;",
                "1690 200\n");
}

// Two `return` edges with different pipelines: the one-digit strings (46
// characters) take the `Select` (10 x 46), the rest the `Where`
// (90 x 8 + 100 x 9): 2080.
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           two_return_edges_spec_29_2_30_1) {
    expectShape("if s.Length > 46 then return s.Where(lambda(c) => c <> 'x'); end; "
                "return s.Select(lambda(c) => c); end AW;",
                "2080 200\n");
}

// A capturing predicate, a two-stage pipeline, and a pipeline bound to a local
// first.
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           capturing_chained_and_local_pipelines_spec_29_2_30_1) {
    expectShape("var k := s.Length; return s.Where(lambda(c) => k > 0 and c <> 'x'); end AW;",
                "1690 200\n");
    expectShape("return s.Where(lambda(c) => c <> 'x').Select(lambda(c) => c); end AW;",
                "1690 200\n");
    expectShape("var q := s.Where(lambda(c) => c <> 'x'); return q; end AW;",
                "1690 200\n");
}

// The expression-bodied method, and a `return await` of another such task.
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           expression_body_and_return_await_spec_29_2_30_1) {
    expectShape("=> s.Where(lambda(c) => c <> 'x');", "1690 200\n");
    expectStrict("  public static async method AW0(s: string): Task<IEnumerable<char>> "
                 "return s.Where(lambda(c) => c <> 'x'); end AW0;\n"
                 "  public static async method AW(s: string): Task<IEnumerable<char>> "
                 "return await AW0(s); end AW;\n",
                 "t := t + (await AW(Make.MkH(i).S)).Count();", "1690 200\n");
}

// A BLOCK-bodied async lambda has a `return` statement, so it is this edge.
// (The expression-bodied async lambda is not: it has no `return` node; #8316.)
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           block_bodied_async_lambda_spec_29_2_30_1) {
    bezel::test::JitResult r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static async method Main(): Task<int>
    var t := 0;
    var f: (string) -> Task<IEnumerable<char>> := async lambda(s) => do return s.Where(lambda(c) => c <> 'x'); end;
    for var i in 0..<200 do
      t := t + (await f(Make.MkH(i).S)).Count();
    end;
    Console.WriteLine($"{t} {Res.Count}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1690 200\n");
}

// The other source families on the same edge, all SEGV on the base. A
// collection the body built is handed to the result (1 + 2 + length:
// 600 + 9490; a Stack / Set of 1 and the length: 200 + 9490); an `Array`
// PARAMETER stays the caller's, eagerly or under a lazy pipeline (200 x 6).
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           collection_results_spec_29_2_30_1) {
    expectFamily("var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(s.Length); return a;",
                 "10090 200\n");
    expectFamily("var l := new Stack<int>(); l.Push(1); l.Push(s.Length); return l;",
                 "9690 200\n");
    expectFamily("var l := new Set<int>(); l.Add(1); l.Add(s.Length); return l;",
                 "9690 200\n");
    expectFamily("return xs;", "1200 200\n");
    expectFamily("return xs.Where(lambda(x) => x > 0);", "1200 200\n");
}

// THE CONTROL: the non-async twin. The sync `return` edge has always performed
// the §29.2 coercion, so this row is green on the base and stays green.
BEZEL_TEST(async_return_ienumerable_coercion_7867,
           sync_twin_where_for_in_control_spec_29_2) {
    expectStrict(kMembers,
                 "var it := NW(Make.MkH(i).S); for var c in it do t := t + 1; end;",
                 "1690 200\n");
}
