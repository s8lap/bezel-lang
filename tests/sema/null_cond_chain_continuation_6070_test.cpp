// Regression test for issue #6070 -- the §7.5 CHAIN CONTINUATION: a plain
// `.` suffix to the right of a `?.` segment (`s?.Length.ToString()`,
// `a?.Count.ToString()`) was REJECTED.
//
// THE DEFECT.
//
//   spec §7.5 (v0.357), *Evaluation*, verbatim:
//
//     "**The chain is the whole postfix sequence** (grammar A.7's
//     `PrimaryExpr`), its plain `.`, `[ ]` and `( )` suffixes included, and
//     a suffix to the right of a `?.` is typed at the value it meets on the
//     path where it runs — the preceding segment's own declared type, never
//     the nullable type the `?.` gives the chain's result: `s?.Length.ToString()`
//     over `s: string?` calls `ToString()` on an `int`, and the chain is a
//     `string?`. A segment whose own declared type is nullable needs its own
//     `?.` (or a `!`) before the next suffix, under §7.4 — `user?.Name?.Length`
//     where `Name` is a `string?` — and parentheses end the chain:
//     `(s?.Length).ToString()` is a member access on an `int?`, §7.4's error."
//
//   Before the fix, the compiler rejected the chain at the §7.4 catch-all
//   ("member access `.ToString` on a nullable expression result requires
//   narrowing to non-null first (§7.4)"), which is the pre-ruling reading of
//   the section. The owner's ruling (v0.355, quoted in the issue) is the
//   reading above: the chain continues, the suffix is typed at the SEGMENT's
//   own declared type, and the chain's result is nullable.
//
// THE ORACLE.
//
//   The VALUE rows assert the EXACT stdout of the AOT program (the real
//   value for a non-null receiver, `null` for a null one) — the strict
//   `runAot` path, not the deprecated in-process JIT harness.
//
//   The REJECTION rows assert the POSITIVE refusal: the program does NOT
//   run and the diagnostic names the §7.4 narrowing rule. They are the
//   shapes §7.5 does NOT admit as a continuation — a segment whose own
//   declared type is nullable, and a parenthesised segment.
//
//   The CONTROL rows assert that the `?.` access ALONE and the plain `.`
//   spelling were never affected -- they pass on BOTH arms, so a "refuse
//   everything" or "lower nothing" regression cannot pass.
//
//   No row asserts the ABSENCE of a diagnostic on a program that ran, so no
//   row is vacuous in the `check_vacuous_tests` sense (#3138).
//
//   THE FIX-FORWARD ROWS (the second half of the file). The first fix
//   admitted the continuation but lowered only a continuation that YIELDS A
//   VALUE: a built-in procedure (`a?.Items.Add(5)`, `.Sort()`, `.Clear()`) was
//   admitted and SILENTLY DROPPED — exit 0, no effect. Those rows pin the
//   effect by the exact stdout, over a non-null AND a null segment (the null
//   path must skip the call and its ARGUMENTS, §7.5), and pin the shapes the
//   continuation does NOT admit by their specific diagnostic.
//
// Cite: spec §7.5 (v0.357), §7.4 (v0.357), §7.6 (v0.357), §21.9 (v0.357),
// §22.3 / §22.4 / §22.5 (v0.357), §28.3 / §28.6 (v0.357); stdlib §9.6, §4.4
// (v0.357).

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"  // runAot / runAotOut
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "public class C "
           "  public static method Pick(present: bool): string? "
           "    if present then "
           "      return \"abc\"; "
           "    end; "
           "    return null; "
           "  end Pick; "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §7.4 narrowing refusal — the diagnostic the chain continuation REPLACES
// for the admitted shapes, and KEEPS for the two §7.5 exclusions.
bool refusedAsNarrowing(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("requires narrowing") != std::string::npos &&
            d.message.find("(\xC2\xA7""7.4)") != std::string::npos)
            return true;
    return false;
}

// The CHECKED (generated-code ASan / LSan) oracle for the rows that allocate
// on the chain's paths: no diagnostic, the program RUNS, exits 0 — a leak or
// a double free is a non-zero exit — and prints exactly `out`. A toolchain
// skip is LOUD, never absorbed.
void expectRunsChecked(const std::string& source, const char* out) {
    auto r = runAotChecked(source);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

// ---------------------------------------------------------------------------
// THE VALUE ROWS -- the chain continues over a non-null and a null receiver.
// This is the issue's own repro (`s?.Length.ToString()`), and the spec's own
// example (`s?.Length.ToString()` over `s: string?` calls `ToString()` on an
// `int`, and the chain is a `string?`).
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_chain_continuation_6070,
           string_length_chain_continues_over_a_non_null_and_a_null_string_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := Pick(true); "
        "    var b := Pick(false); "
        "    var ra: string? := a?.Length.ToString(); "
        "    var rb: string? := b?.Length.ToString(); "
        "    Console.WriteLine(ra ?? \"null\"); "
        "    Console.WriteLine(rb ?? \"null\"); ")),
        "3\nnull");
}

// The COLLECTION form of the issue's repro: `a?.Count.ToString()` over an
// `Array<int>?`.
BEZEL_TEST(null_cond_chain_continuation_6070,
           array_count_chain_continues_over_a_non_null_and_a_null_array_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a: Array<int>? := [1, 2]; "
        "    var b: Array<int>? := null; "
        "    var ra: string? := a?.Count.ToString(); "
        "    var rb: string? := b?.Count.ToString(); "
        "    Console.WriteLine(ra ?? \"null\"); "
        "    Console.WriteLine(rb ?? \"null\"); ")),
        "2\nnull");
}

// The READ spelling of the continuation: `x?.Y.Z`, where `Z` is a PROPERTY
// read at the SEGMENT's own declared type (the owner's own MEASURED example
// on the issue — `e?.Key.Length` — has this shape). The segment is `Name`, a
// `string`, so `.Length` is read on a `string` — NOT on the `string?` the
// `?.` gives the chain's result.
BEZEL_TEST(null_cond_chain_continuation_6070,
           read_continuation_is_typed_at_the_segments_own_type_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "public class Box public Name: string := \"abc\"; end Box; "
        "public class C "
        "  public static method Main(): int "
        "    var a: Box? := new Box(); "
        "    var b: Box? := null; "
        "    var ra: int? := a?.Name.Length; "
        "    var rb: int? := b?.Name.Length; "
        "    Console.WriteLine(ra ?? -1); "
        "    Console.WriteLine(rb ?? -1); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "3\n-1");
}

// The chain's RESULT is nullable (§7.5): the `string?` annotation is
// admitted, and a NON-nullable one is refused by §7 (the nullability gate).
BEZEL_TEST(null_cond_chain_continuation_6070,
           chain_result_is_a_nullable_string_7_5) {
    auto r = runAot(prog(
        "    var a := Pick(true); "
        "    var r: string := a?.Length.ToString(); "
        "    return 0; "));
    // The SPECIFIC refusal — §7's nullability gate on the initializer — and
    // NOT §7.4's narrowing refusal of the chain itself, which is what the
    // pre-fix compiler reports for this very program (so `hasErrors()` alone
    // was green on both arms).
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the initializer for `r` is nullable but the target type is "
           "non-nullable"));
    BEZEL_EXPECT_FALSE(refusedAsNarrowing(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The chain's result is USABLE as a nullable value: the `??` operand is the
// default when the segment is null.
BEZEL_TEST(null_cond_chain_continuation_6070,
           chain_result_coalesces_to_the_default_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := Pick(false); "
        "    var r: string? := a?.Length.ToString(); "
        "    Console.WriteLine(r ?? \"fallback\"); ")),
        "fallback");
}

// ---------------------------------------------------------------------------
// THE REJECTION ROWS -- the two shapes §7.5 does NOT admit as a chain
// continuation.
// ---------------------------------------------------------------------------

// §7.5: "A segment whose own declared type is nullable needs its own `?.`
// (or a `!`) before the next suffix, under §7.4." `Name` is declared
// `string?`, so `b?.Name.Length` is a member access on a nullable value.
BEZEL_TEST(null_cond_chain_continuation_6070,
           nullable_segment_needs_its_own_null_conditional_7_4_7_5) {
    auto r = runAot(
        "module M; import System; "
        "public class Box public Name: string? := null; end Box; "
        "public class C "
        "  public static method Main(): int "
        "    var b: Box? := new Box(); "
        "    var n := b?.Name.Length; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(refusedAsNarrowing(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §7.5: "parentheses end the chain: `(s?.Length).ToString()` is a member
// access on an `int?`, §7.4's error."
BEZEL_TEST(null_cond_chain_continuation_6070,
           parenthesised_segment_ends_the_chain_7_4_7_5) {
    auto r = runAot(prog(
        "    var a := Pick(true); "
        "    var r: string? := (a?.Length).ToString(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(refusedAsNarrowing(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// THE CONTROL ROWS -- the `?.` access ALONE and the plain `.` spelling are
// untouched (they pass on BOTH arms).
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_chain_continuation_6070,
           null_cond_access_alone_is_unchanged_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := Pick(true); "
        "    var n: int? := a?.Length; "
        "    Console.WriteLine(n ?? -1); ")),
        "3");
}

BEZEL_TEST(null_cond_chain_continuation_6070,
           plain_access_spelling_is_unchanged_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := Pick(true); "
        "    var n: int := a!.Length; "
        "    Console.WriteLine(n); ")),
        "3");
}

// ===========================================================================
// THE FIX-FORWARD ROWS — a continuation with NO VALUE runs (§7.5, v0.357).
//
// §7.5's own example of a call the chain performs for its effect is
// `user?.Log("Accessed");`. A built-in procedure as the continuation is the
// same statement one segment further along, and the first fix admitted it
// and emitted NOTHING for it.
// ===========================================================================

// The reviewer's isolating probe (q07c_isolate.bzl), verbatim. Before the
// fix-forward it printed `after add: 0 / k: 0 / c: False / after 3: 0`.
BEZEL_TEST(null_cond_chain_continuation_6070,
           void_builtin_add_continuation_runs_on_the_non_null_path_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Box
  public owned Items: Array<int> := [];
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    a?.Items.Add(5);
    Console.WriteLine($"after add: {a!.Items.Count}");
    var k: int? := a?.Items.Count;
    Console.WriteLine($"k: {k}");
    var c: bool? := a?.Items.Contains(5);
    Console.WriteLine($"c: {c}");
    a?.Items.Add(6);
    a?.Items.Add(7);
    Console.WriteLine($"after 3: {a!.Items.Count}");
  end Main;
end Program;
)bzl"),
        "after add: 1\nk: 1\nc: True\nafter 3: 3");
}

// The reviewer's q14_void_builtins.bzl, verbatim: `Sort()` and `Clear()`.
// Before the fix-forward: `sorted first: 3 / count after clear: 3`.
BEZEL_TEST(null_cond_chain_continuation_6070,
           void_builtin_sort_and_clear_continuations_run_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Box
  public owned Items: Array<int> := [3, 1, 2];
  public owned Sb: StringBuilder := new StringBuilder();
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    a?.Items.Sort();
    Console.WriteLine($"sorted first: {a!.Items[0]}");
    a?.Items.Clear();
    Console.WriteLine($"count after clear: {a!.Items.Count}");
    a?.Sb.Append("xy");
    Console.WriteLine($"sb: {a!.Sb.ToString()}");
  end Main;
end Program;
)bzl"),
        "sorted first: 1\ncount after clear: 0\nsb: xy");
}

// The reviewer's q13_builtin_methods.bzl, verbatim: value-yielding built-in
// continuations beside a void one. Before the fix-forward the last line was
// `count after add: 3`.
BEZEL_TEST(null_cond_chain_continuation_6070,
           value_and_void_builtin_continuations_side_by_side_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Box
  public Name: string := "Hello";
  public owned Items: Array<int> := [1, 2, 3];
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var u: string? := a?.Name.ToUpper();
    Console.WriteLine($"upper: {u}");
    var s: string? := a?.Name.Substring(1);
    Console.WriteLine($"substr: {s}");
    var st: bool? := a?.Name.StartsWith("He");
    Console.WriteLine($"starts: {st}");
    var ct: bool? := a?.Items.Contains(2);
    Console.WriteLine($"contains2: {ct}");
    var ix: int? := a?.Items.IndexOf(3);
    Console.WriteLine($"indexof3: {ix}");
    var cnt: int? := a?.Items.Count;
    Console.WriteLine($"count: {cnt}");
    var plain: bool := a!.Items.Contains(2);
    Console.WriteLine($"plain contains2: {plain}");
    a?.Items.Add(9);
    Console.WriteLine($"count after add: {a!.Items.Count}");
  end Main;
end Program;
)bzl"),
        "upper: HELLO\nsubstr: ello\nstarts: True\ncontains2: True\n"
        "indexof3: 2\ncount: 3\nplain contains2: True\ncount after add: 4");
}

// §7.5: over a null segment "no call is performed, and the argument and index
// expressions of the skipped suffixes are not evaluated either". `Side(2)` and
// `Side(8)` sit on the null path and must print nothing; the non-null path
// evaluates its arguments left to right, once.
BEZEL_TEST(null_cond_chain_continuation_6070,
           void_continuation_and_its_arguments_are_skipped_over_a_null_segment_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Box
  public owned Items: Array<int> := [];
end Box;
public class Program
  private static method Side(v: int): int
    Console.WriteLine($"side {v}");
    return v;
  end Side;
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    a?.Items.Add(Side(1));
    n?.Items.Add(Side(2));
    a?.Items.Insert(Side(0), Side(7));
    n?.Items.Insert(Side(0), Side(8));
    Console.WriteLine($"count: {a!.Items.Count} first: {a!.Items[0]}");
  end Main;
end Program;
)bzl"),
        "side 1\nside 0\nside 7\ncount: 2 first: 7");
}

// The same statement over the other built-in collections: `Dictionary.Add`,
// `Clear`, `Queue.Enqueue`, `Stack.Push`, `LinkedList.AddLast`/`AddFirst`,
// and the value-yielding `Set.Add` / `StringBuilder.Append` used as bare
// statements. Before the fix-forward the first five counts were all 0.
BEZEL_TEST(null_cond_chain_continuation_6070,
           void_continuations_on_the_other_built_in_collections_7_5) {
    expectRunsChecked(R"bzl(
module Conf;

public class Box
  public owned D: Dictionary<string, int> := new Dictionary<string, int>();
  public owned S: Set<int> := new Set<int>();
  public owned Q: Queue<int> := new Queue<int>();
  public owned K: Stack<int> := new Stack<int>();
  public owned Sb: StringBuilder := new StringBuilder();
  public owned L: LinkedList<int> := new LinkedList<int>();
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    a?.D.Add("k", 1);
    Console.WriteLine($"dict: {a!.D.Count}");
    a?.D.Clear();
    Console.WriteLine($"dict clear: {a!.D.Count}");
    a?.S.Add(4);
    Console.WriteLine($"set: {a!.S.Count}");
    a?.Q.Enqueue(4);
    Console.WriteLine($"queue: {a!.Q.Count}");
    a?.K.Push(4);
    Console.WriteLine($"stack: {a!.K.Count}");
    a?.Sb.Append("xy");
    a?.Sb.AppendLine("z");
    Console.WriteLine($"sb: {a!.Sb.Length}");
    a?.Sb.Clear();
    Console.WriteLine($"sb clear: {a!.Sb.Length}");
    a?.L.AddLast(3);
    a?.L.AddFirst(2);
    Console.WriteLine($"llist: {a!.L.Count}");
    n?.D.Add("k", 1);
    n?.S.Add(4);
    n?.Q.Enqueue(4);
    n?.K.Push(4);
    n?.Sb.Append("xy");
    n?.L.AddLast(3);
    Console.WriteLine("null ok");
  end Main;
end Program;
)bzl",
        "dict: 1\ndict clear: 0\nset: 1\nqueue: 1\nstack: 1\nsb: 4\n"
        "sb clear: 0\nllist: 2\nnull ok\n");
}

// The statement in every position a statement can stand: a method body, an
// `if` arm, a loop body, an expression-bodied lambda — and over a nullable
// PARAMETER as the receiver.
BEZEL_TEST(null_cond_chain_continuation_6070,
           void_continuation_in_nested_statement_positions_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Box
  public owned Items: Array<int> := [];
  public Name: string := "nm";
end Box;
public class Program
  private static method Push(b: Box?, v: int)
    b?.Items.Add(v);
  end Push;
  private static method Len(b: Box?): int => b?.Name.Length ?? -1;
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    Push(a, 1);
    Push(a, 2);
    Push(n, 3);
    Console.WriteLine($"count: {a!.Items.Count}");
    Console.WriteLine($"len: {Len(a)} {Len(n)}");
    if a?.Name.StartsWith("n") ?? false then
      a?.Items.Add(9);
    end;
    var i := 0;
    while i < 3 do
      a?.Items.Add(i);
      i := i + 1;
    end;
    Console.WriteLine($"count2: {a!.Items.Count}");
    const act := lambda() => a?.Items.Clear();
    act();
    Console.WriteLine($"count3: {a!.Items.Count}");
  end Main;
end Program;
)bzl"),
        "count: 2\nlen: 2 -1\ncount2: 6\ncount3: 0");
}

// 300 iterations with a FRESH OWNED argument and a fresh string argument on
// both paths: over the null segment the arguments are never produced (so
// nothing is left to reclaim), over the non-null one they are stored once.
// Under the sanitizer leg this row is the leak oracle for the statement form.
BEZEL_TEST(null_cond_chain_continuation_6070,
           void_continuation_with_fresh_owned_arguments_300_times_7_5) {
    expectRunsChecked(R"bzl(
module Conf;

public class Res
  public Id: int := 1;
end Res;
public class Box
  public owned Items: OwningArray<Res> := new OwningArray<Res>();
  public owned Names: Array<string> := [];
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    var total := 0;
    for var i in 0..<300 do
      a?.Items.Add(new Res());
      n?.Items.Add(new Res());
      a?.Names.Add("n" + i.ToString());
      n?.Names.Add("n" + i.ToString());
    end;
    total := a!.Items.Count + a!.Names.Count;
    Console.WriteLine($"total={total}");
  end Main;
end Program;
)bzl",
        "total=600\n");
}

// A `ref` argument on the continuation: the callee runs once over the
// non-null segment and not at all over the null one.
BEZEL_TEST(null_cond_chain_continuation_6070,
           ref_argument_continuation_runs_only_on_the_non_null_path_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Inner
  public method Bump(ref v: int)
    v := v + 1;
  end Bump;
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    var v := 10;
    a?.In.Bump(ref v);
    n?.In.Bump(ref v);
    Console.WriteLine($"v: {v}");
  end Main;
end Program;
)bzl"),
        "v: 11");
}

// §22.5: a NAMED argument on a built-in continuation binds by NAME. The first
// fix lost the binding on the way to codegen and lowered the arguments in
// parameter order against swapped slots — `sub2` printed `l`.
BEZEL_TEST(null_cond_chain_continuation_6070,
           named_argument_on_a_builtin_continuation_binds_by_name_22_5) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Box
  public Name: string := "Hello";
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var s: string? := a?.Name.Substring(startIndex := 1, length := 2);
    Console.WriteLine($"sub: {s}");
    var t: string? := a?.Name.Substring(length := 2, startIndex := 1);
    Console.WriteLine($"sub2: {t}");
    var p: string := a!.Name.Substring(length := 2, startIndex := 1);
    Console.WriteLine($"plain: {p}");
  end Main;
end Program;
)bzl"),
        "sub: el\nsub2: el\nplain: el");
}

// ===========================================================================
// THE FIX-FORWARD ROWS — the chain's result is nullable for EVERY result
// type (§7.5, §7.6, v0.357).
// ===========================================================================

// A FUNCTION-typed continuation result is lifted like any other: it is
// `null` over a null segment, so it is called with `?(..)`.
BEZEL_TEST(null_cond_chain_continuation_6070,
           function_valued_continuation_result_is_null_over_a_null_segment_7_6) {
    BEZEL_EXPECT_EQ(runAotOut(R"bzl(
module Conf;

public class Inner
  public H: (int) -> int := lambda(x: int) => x + 10;
  public method GetH(): (int) -> int
    return lambda(x: int) => x + 1;
  end GetH;
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    var f := a?.In.GetH();
    var g := n?.In.GetH();
    Console.WriteLine(f?(1) ?? -1);
    Console.WriteLine(g?(1) ?? -1);
    var p := a?.In.H;
    var q := n?.In.H;
    Console.WriteLine(p?(1) ?? -1);
    Console.WriteLine(q?(1) ?? -1);
  end Main;
end Program;
)bzl"),
        "2\n-1\n11\n-1");
}

// ... and a PLAIN call of it is §7.4's error. The first fix typed the
// function-valued chain non-nullable, admitted `g(1)`, and the program died
// with SIGSEGV on the null `g`.
BEZEL_TEST(null_cond_chain_continuation_6070,
           plain_call_of_a_function_valued_chain_result_is_refused_7_4_7_6) {
    auto viaCall = runAot(R"bzl(
module Conf;

public class Inner
  public method GetH(): (int) -> int
    return lambda(x: int) => x + 1;
  end GetH;
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var n: Box? := null;
    var g := n?.In.GetH();
    Console.WriteLine(g(1));
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        viaCall, "call through a nullable function value `g` requires "
                 "narrowing it to non-null first"));
    BEZEL_EXPECT_FALSE(viaCall.ran);

    auto viaField = runAot(R"bzl(
module Conf;

public class Inner
  public H: (int) -> int := lambda(x: int) => x + 1;
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var n: Box? := null;
    var g := n?.In.H;
    Console.WriteLine(g(1));
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        viaField, "call through a nullable function value `g` requires "
                  "narrowing it to non-null first"));
    BEZEL_EXPECT_FALSE(viaField.ran);
}

// ===========================================================================
// THE FIX-FORWARD ROWS — what the continuation does NOT admit, each by its
// SPECIFIC diagnostic. Every one of these compiled after the first fix.
// ===========================================================================

// A bare METHOD REFERENCE as the continuation (`x?.Y.M`, no call) is not a
// value the chain can yet lift: it keeps §7.4's refusal. The first fix
// admitted it as a non-nullable function value and `g(1)` over a null `n`
// died with SIGSEGV.
BEZEL_TEST(null_cond_chain_continuation_6070,
           bare_method_reference_continuation_keeps_the_narrowing_refusal_7_4) {
    auto r = runAot(R"bzl(
module Conf;

public class Inner
  public method Twice(x: int): int => x * 2;
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var n: Box? := null;
    var g := n?.In.Twice;
    Console.WriteLine(g(1));
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(refusedAsNarrowing(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "member access `.Twice`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §22.4: an `out` argument is assigned on every normal path of the call —
// and over a null segment the call does not run. The first fix admitted the
// inline `out var` form and the program read the recipient UNINITIALIZED
// (it printed 92523904), and admitted the `out owned` form the direct `?.`
// call keeps refused (#2393).
BEZEL_TEST(null_cond_chain_continuation_6070,
           out_argument_on_a_continuation_call_is_refused_22_4) {
    const char* needle =
        "an `out` argument is not admitted on a call that continues a "
        "null-conditional chain";
    auto inlineVar = runAot(R"bzl(
module Conf;

public class Inner
  public method TryGet(out v: int): bool
    v := 5;
    return true;
  end TryGet;
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var n: Box? := null;
    var ok: bool? := n?.In.TryGet(out var v);
    Console.WriteLine($"{ok ?? false} {v}");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(inlineVar, needle));
    BEZEL_EXPECT_FALSE(inlineVar.ran);

    auto builtin = runAot(R"bzl(
module Conf;

public class Box
  public owned D: Dictionary<string, int> := new Dictionary<string, int>();
end Box;
public class Program
  public static method Main()
    var n: Box? := null;
    var ok: bool? := n?.D.TryGetValue("k", out var v);
    Console.WriteLine($"{ok ?? false} {v}");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(builtin, needle));
    BEZEL_EXPECT_FALSE(builtin.ran);

    auto outOwned = runAot(R"bzl(
module Conf;

public class Res
  public Id: int := 1;
end Res;
public class Inner
  public method Make(out owned r: Res)
    r := new Res();
  end Make;
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var n: Box? := null;
    n?.In.Make(out using var r);
    Console.WriteLine("after");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(outOwned, needle));
    BEZEL_EXPECT_FALSE(outOwned.ran);
}

// §7.5 / §28.3: a `move` argument on the continuation is a CONDITIONAL
// transfer — over a null segment the argument is not evaluated, so the
// binding is neither transferred nor disposed. The first fix admitted it and
// the binding leaked (its destructor never ran).
BEZEL_TEST(null_cond_chain_continuation_6070,
           move_argument_on_a_continuation_call_is_refused_28_3) {
    auto r = runAot(R"bzl(
module Conf;

public class Res
  public Id: int := 1;
  destructor
    Console.WriteLine("res dtor");
  end;
end Res;
public class Inner
  public owned Held: Res? := null;
  public method Take(owned r: Res)
    Held := r;
  end Take;
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var n: Box? := null;
    var r := new Res();
    n?.In.Take(move r);
    Console.WriteLine("after");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `move` argument is not admitted on a call that continues a "
           "null-conditional chain"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §7.5 / §28.3 (v0.357): the same conditional transfer, with the `move` one
// level down inside the argument. Without the refusal `n?.In.Take(Wrap(move
// r))` over a null `n` leaked `r` exactly as the whole-argument spelling did
// (LeakSanitizer, 16 bytes).
BEZEL_TEST(null_cond_chain_continuation_6070,
           move_nested_in_a_continuation_call_argument_is_refused_28_3) {
    auto r = runAot(R"bzl(
module Conf;

public class Res
  public Id: int := 1;
  destructor
    Console.WriteLine("res dtor");
  end;
end Res;
public class Inner
  public owned Held: Res? := null;
  public method Take(owned r: Res)
    Held := r;
  end Take;
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  private static method Wrap(owned r: Res): owned Res
    return r;
  end Wrap;
  public static method Main()
    var n: Box? := null;
    var r := new Res();
    n?.In.Take(Wrap(move r));
    Console.WriteLine("after");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a `move` argument is not admitted on a call that continues a "
           "null-conditional chain"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §7.5 / §28.3 (v0.357): the consume-if-selected marker (A.7's `owned r` in
// an if-expression branch) stays ADMITTED in a continuation call's argument,
// because that form records at run time whether the transfer happened. Over
// the non-null `a` each `r` is transferred into `Held` (299 replaced, the
// last disposed with `a` after the print); over the null `n` the argument is
// not evaluated (§7.5) and each `s` is disposed at its scope's end (300).
BEZEL_TEST(null_cond_chain_continuation_6070,
           consume_if_selected_argument_is_disposed_once_on_both_paths_300_times_7_5) {
    expectRunsChecked(R"bzl(
module Conf;

public class Counter
  public static Dtors: int := 0;
end Counter;
public class Res
  public Id: int := 1;
  destructor
    Counter.Dtors := Counter.Dtors + 1;
  end;
end Res;
public class Inner
  public owned Held: Res? := null;
  public method Take(owned r: Res)
    Held := r;
  end Take;
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    for var i in 0..<300 do
      var r := new Res();
      var s := new Res();
      a?.In.Take((if i >= 0 then owned r else new Res()));
      n?.In.Take((if i >= 0 then owned s else new Res()));
    end;
    Console.WriteLine($"dtors={Counter.Dtors}");
  end Main;
end Program;
)bzl",
        "dtors=599\n");
}

// §28.6 "Who may call Dispose()": the `?.` segment is a field read, a borrow
// the caller does not own. The `.` spelling `a.R.Dispose();` is the ownership
// pass's error; the first fix admitted the chain spelling and ran the
// destructor from outside the declaring type.
BEZEL_TEST(null_cond_chain_continuation_6070,
           dispose_continuation_on_a_field_segment_is_refused_28_6) {
    auto r = runAot(R"bzl(
module Conf;

public class Res
  public Id: int := 1;
  destructor
    Console.WriteLine("res dtor");
  end;
end Res;
public class Box
  public owned R: Res := new Res();
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    a?.R.Dispose();
    Console.WriteLine("after");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot call `Dispose()` on the `?.` segment of a "
           "null-conditional chain"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §21.9: a continuation call that produces an OWNED value needing disposal
// may not be discarded as a statement — exactly as its `.` spelling
// (`a.In.Make();`) may not. The first fix admitted it and every result
// leaked. The owning-collection drain (`PopLast`) is the built-in form.
BEZEL_TEST(null_cond_chain_continuation_6070,
           discarded_owned_continuation_result_is_refused_21_9) {
    const char* needle =
        "this expression produces an owned value that requires disposal";
    auto userCall = runAot(R"bzl(
module Conf;

public class Res
  public Id: int := 1;
  destructor
    Console.WriteLine("res dtor");
  end;
end Res;
public class Inner
  public method Make(): owned Res => new Res();
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    a?.In.Make();
    Console.WriteLine("end");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(userCall, needle));
    BEZEL_EXPECT_FALSE(userCall.ran);

    auto drain = runAot(R"bzl(
module Conf;

public class Res
  public Id: int := 1;
  destructor
    Console.WriteLine("res dtor");
  end;
end Res;
public class Box
  public owned Pool: OwningArray<Res> := new OwningArray<Res>();
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    a?.Pool.Add(new Res());
    a?.Pool.PopLast();
    Console.WriteLine("end");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(drain, needle));
    BEZEL_EXPECT_FALSE(drain.ran);
}

// ... and BOUND, the same owned result is disposed exactly once per
// iteration over the non-null segment and never produced over the null one
// (the destructor count is the oracle: 300, not 0 and not 600).
BEZEL_TEST(null_cond_chain_continuation_6070,
           bound_owned_continuation_result_is_disposed_once_300_times_7_5) {
    expectRunsChecked(R"bzl(
module Conf;

public class Counter
  public static Dtors: int := 0;
end Counter;
public class Res
  public Id: int := 1;
  destructor
    Counter.Dtors := Counter.Dtors + 1;
  end;
end Res;
public class Inner
  public method Make(): owned Res => new Res();
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    for var i in 0..<300 do
      using var m := a?.In.Make();
      using var z := n?.In.Make();
    end;
    Console.WriteLine($"dtors={Counter.Dtors}");
  end Main;
end Program;
)bzl",
        "dtors=300\n");
}

// A continuation with NO VALUE is not a value: binding it is refused with
// the built-in's own "produced no value" diagnostic, as the `.` spelling is.
BEZEL_TEST(null_cond_chain_continuation_6070,
           void_builtin_continuation_cannot_initialize_a_local_7_5) {
    auto r = runAot(R"bzl(
module Conf;

public class Box
  public owned Items: Array<int> := [];
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var x := a?.Items.Add(5);
    Console.WriteLine($"count: {a!.Items.Count}");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Add` on built-in type `Array` produced no value"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The continuation's OWN errors are still its own: a wrong argument type on
// the void built-in is the `.` spelling's diagnostic, not a silent drop and
// not §7.4's narrowing refusal.
BEZEL_TEST(null_cond_chain_continuation_6070,
           void_builtin_continuation_keeps_its_own_argument_diagnostic_5_1) {
    auto r = runAot(R"bzl(
module Conf;

public class Box
  public owned Items: Array<int> := [];
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    a?.Items.Add("str");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `Array<T>.Add` has a type that is not implicitly "
           "convertible to the target type"));
    BEZEL_EXPECT_FALSE(refusedAsNarrowing(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §21.9's other half: a continuation call whose owned result is of a
// TRIVIAL-dispose class is reclaimed where the statement discards it — over
// the non-null segment once per statement, over the null one there is
// nothing to reclaim (the null-guarded free) — and the `_ :=` sink disposes a
// destructor-bearing result exactly once. 300 iterations under the checked
// oracle: a leak or a double free is a non-zero exit.
BEZEL_TEST(null_cond_chain_continuation_6070,
           discarded_trivial_owned_continuation_result_is_reclaimed_300_times_21_9) {
    expectRunsChecked(R"bzl(
module Conf;

public class Counter
  public static Dtors: int := 0;
end Counter;
public class Res
  public Id: int := 1;
  destructor
    Counter.Dtors := Counter.Dtors + 1;
  end;
end Res;
public class Plain
  public Id: int := 2;
end Plain;
public class Inner
  public method Make(): owned Res => new Res();
  public method MakePlain(): owned Plain => new Plain();
  public method MaybePlain(): owned Plain? => new Plain();
end Inner;
public class Box
  public owned In: Inner := new Inner();
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    for var i in 0..<300 do
      a?.In.MakePlain();
      n?.In.MakePlain();
      a?.In.MaybePlain();
      _ := a?.In.MakePlain();
      _ := n?.In.MakePlain();
      _ := a?.In.Make();
      _ := n?.In.Make();
    end;
    Console.WriteLine($"dtors={Counter.Dtors}");
  end Main;
end Program;
)bzl",
        "dtors=300\n");
}

// A BUILT-IN method named WITHOUT a call as the continuation
// (`a?.Name.ToUpper`). Sema types the bare built-in name as the method's
// RESULT (the `.` spelling's own hole — `var f := a.Name.ToUpper;` compiles
// and the program dies with SIGSEGV), so the chain would promise a `string?`
// it has no value for: the first fix compiled it and `f` read `null` over a
// NON-null `a`. The chain refuses it, by name.
BEZEL_TEST(null_cond_chain_continuation_6070,
           bare_builtin_method_name_continuation_is_refused_7_5) {
    auto r = runAot(R"bzl(
module Conf;

public class Box
  public Name: string := "abc";
end Box;
public class Program
  public static method Main()
    var a: Box? := new Box();
    var f := a?.Name.ToUpper;
    Console.WriteLine($"{f}");
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the continuation `.ToUpper` of this null-conditional chain does "
           "not lower to a value"));
    BEZEL_EXPECT_FALSE(r.ran);
}
