// An iterator frame (sync or async) holds its own reference to every `string`
// it captures: a plain `string` / `string?` parameter and the `this` of an
// `extension string` member (fix #7798).
//
// §29.3 (v0.350): an iterator's "arguments are captured into a heap
// state-machine frame that outlives the synchronous call". §29.4 (v0.350): "An
// iterator method's body does not run when the method is called ... the body
// runs incrementally as the consumer calls `MoveNext()`". §28.9 (v0.350):
// `string` is runtime-managed, "the type's lifetime is managed by the runtime
// without user involvement". §9.4 / §30.1 (v0.350): "An async or iterator
// member holds the fixed receiver value in its frame". So a string the frame
// captured must stay valid for as long as the frame can read it, however the
// caller's argument is reclaimed: a +1 temporary is released when the call
// returns (the #429 argument-pass release), a named local at its scope's end,
// and a field read's receiver at the full expression's end.
//
// Before the fix the frame stored the caller's pointer and took no reference,
// so every MoveNext read it after the caller had freed it. MEASURED on the base
// (26216e9f), bezelc --sanitize=address, loop x100: `for var x in Gen("..." +
// n.ToString())`, `Gen(Make.MkH(i).Name())`, an instance, generic, `string?`
// or two-string iterator, re-enumerating, breaking early, forwarding the
// parameter to an inner iterator, returning the frame out of a local's scope,
// an `extension string` receiver, and the async-iterator twins -> every one a
// `heap-use-after-free` (one SEGV) in `bz$iter$...$MoveNext` /
// `$MoveNextAsync`'s resume.
//
// THE FIX. The frame's construction takes a +1 on each such capture
// (emitIteratorMethod / emitAsyncIteratorMethod, beside the #1864 / #2609
// closure-capture retain) and its Dispose drops it. A GetEnumerator /
// GetAsyncEnumerator clone takes its own +1 too and its own Dispose drops
// it: an explicit `Gen(s).GetEnumerator()` hands out a clone that outlives
// its source frame (§29.11). A plain `async` method already holds its
// string parameters (#5262-ff).
//
// THE ORACLE. Every row is `runAotChecked` (generated-code ASan + LSan), loops
// 100 times over heap strings built at run time (literals are rc sentinels
// and cannot dangle), and prints the sum of the values read through the
// string plus the destructor count of the temporaries' `Res` (stdout, not
// the exit code). A use-after-free, a double release and a leaked reference
// each fail the row. The never-enumerated, literal, `null` and named-local
// rows are controls that passed on the base: they pin that the new retain is
// released exactly once on every path.

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
  public constructor(n: int) S := "heap-" + n.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"; end constructor;
  public method Name(): string => S;
end H;
public class Make public static method MkH(n: int): owned H return new H(n); end MkH; end Make;
extension string
  public method Each(): IEnumerable<int> for var k in 0..<3 do yield return this.Length + k; end; end Each;
  public async method AEach(): IAsyncEnumerable<int> for var k in 0..<3 do await Task.Delay(0); yield return this.Length + k; end; end AEach;
end;
public class Inst
  public Base: int := 1;
  public method Gen(s: string): IEnumerable<int> for var k in 0..<3 do yield return s.Length + k + Base; end; end Gen;
end Inst;
public class Program
  public static method Gen(s: string): IEnumerable<int> for var k in 0..<3 do yield return s.Length + k; end; end Gen;
  public static method GenN(s: string?): IEnumerable<int> for var k in 0..<3 do yield return (s ?? "").Length + k; end; end GenN;
  public static method Gen2(a: string, n: int, b: string): IEnumerable<int> for var k in 0..<3 do yield return a.Length + b.Length + n + k; end; end Gen2;
  public static method GenT<T>(v: T, s: string): IEnumerable<int> for var k in 0..<3 do yield return s.Length + k; end; end GenT;
  public static method Outer(s: string): IEnumerable<int> for var x in Gen(s) do yield return x + 1; end; end Outer;
  public static method Mk(i: int): IEnumerable<int>
    var s := "heap-" + i.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    return Gen(s);
  end Mk;
  public static method MkEach(i: int): IEnumerable<int>
    var s := "heap-" + i.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    return s.Each();
  end MkEach;
  public static method MkAEach(i: int): IAsyncEnumerable<int>
    var s := "heap-" + i.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    return s.AEach();
  end MkAEach;
  public static method MkAIt(i: int): IAsyncEnumerable<int>
    var s := "heap-" + i.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    return Program.AIt(s);
  end MkAIt;
  public static async method AIt(s: string): IAsyncEnumerable<int> for var k in 0..<3 do await Task.Delay(0); yield return s.Length + k; end; end AIt;
  public static method En(i: int): IEnumerator<int>
    var s := "heap-" + i.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    return Gen(s).GetEnumerator();
  end En;
  public static method EnArg(s: string): IEnumerator<int> return Gen(s).GetEnumerator(); end EnArg;
)";

// `body` runs 100 times inside a synchronous Main; prints "<t> <Res.Count>".
std::string syncProgram(const std::string& body) {
    return std::string(kPrelude) + R"(
  public static method Main(): int
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

// The `async Main` twin, for the `await for` rows.
std::string asyncProgram(const std::string& body) {
    return std::string(kPrelude) + R"(
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

void expectRun(const std::string& program, const std::string& expected) {
    bezel::test::JitResult r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// The argument "heap-" + i + "-" + 27 x is 34 characters for a one-digit i and
// 35 for two digits; per call Gen yields len, len+1, len+2 (3*len + 3). Over
// i = 0..99: 10*(3*34+3) + 90*(3*35+3) = 1050 + 9720 = 10770.
const char* kConcat = R"("heap-" + i.ToString() + "-xxxxxxxxxxxxxxxxxxxxxxxxxxx")";

std::string withConcat(const std::string& pre, const std::string& post) {
    return pre + kConcat + post;
}

}  // namespace

// The issue's own repro (r37-rev-u7728/p/mini_iter_arg_concat.bzl): a +1 concat
// passed straight to an iterator in a `for`. 46 chars -> 46 + 47 + 48.
BEZEL_TEST(iterator_frame_string_retain, issue_repro_concat_arg_in_for_spec_29_4_7798) {
    expectRun(R"(
module M;
public class Program
  public static method Gen(s: string): IEnumerable<int> for var k in 0..<3 do yield return s.Length + k; end; end Gen;
  public static method Main(): int
    var t := 0;
    for var x in Gen("heap-1-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" + 1.ToString()) do t := t + x; end;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
)", "144\n");
}

// The issue's second shape: a string returned by a method of a temporary
// (`Gen(MkH(i).Name())`), in a `for`. The receiver is disposed (count 100).
BEZEL_TEST(iterator_frame_string_retain, method_string_of_temporary_in_for_spec_29_4_7798) {
    expectRun(syncProgram("for var x in Gen(Make.MkH(i).Name()) do t := t + x; end;"),
              "14370 100\n");
}

// The frame bound first, then enumerated: the +1 is released at the call's end.
BEZEL_TEST(iterator_frame_string_retain, concat_arg_bound_then_enumerated_spec_29_4_7798) {
    expectRun(syncProgram(withConcat("var it := Gen(", "); for var x in it do t := t + x; end;")),
              "10770 0\n");
}

// The concat as one arm of an if-expression argument (the reviewed shape
// r37-rev-u7728b/p2/c01_base_ifexpr_concat.bzl). Odd i read "lit" (3 chars).
BEZEL_TEST(iterator_frame_string_retain, ifexpr_concat_arg_spec_29_4_7798) {
    expectRun(syncProgram(withConcat("var it := Gen((if i mod 2 = 0 then ",
                                     " else \"lit\")); for var x in it do t := t + x; end;")),
              "5985 0\n");
}

// An instance iterator method (a `this` capture beside the string one).
BEZEL_TEST(iterator_frame_string_retain, instance_iterator_method_spec_29_4_7798) {
    expectRun(syncProgram(withConcat("var o := new Inst(); var it := o.Gen(",
                                     "); for var x in it do t := t + x; end;")),
              "11070 0\n");
}

// A generic iterator: a concrete `string` parameter beside a `T`, and `T`
// itself substituted by `string`.
BEZEL_TEST(iterator_frame_string_retain, generic_iterator_string_param_spec_29_4_7798) {
    expectRun(syncProgram(withConcat("var it := GenT<int>(i, ",
                                     "); for var x in it do t := t + x; end;")),
              "10770 0\n");
    expectRun(syncProgram(withConcat("var it := GenT<string>(\"v\" + i.ToString(), ",
                                     "); for var x in it do t := t + x; end;")),
              "10770 0\n");
}

// A `string?` parameter, given a heap string and (control) `null`.
BEZEL_TEST(iterator_frame_string_retain, nullable_string_param_spec_29_4_7798) {
    expectRun(syncProgram(withConcat("var it := GenN(", "); for var x in it do t := t + x; end;")),
              "10770 0\n");
    expectRun(syncProgram("var it := GenN(null); for var x in it do t := t + x; end;"),
              "300 0\n");
}

// Two string parameters around an int, one a concat and one a method string of
// a temporary.
BEZEL_TEST(iterator_frame_string_retain, two_string_params_spec_29_4_7798) {
    expectRun(syncProgram("var it := Gen2(\"heap-\" + i.ToString() + \"-xxxxxxxxxxxxxxxxx\", i, "
                          "Make.MkH(i).Name()); for var x in it do t := t + x; end;"),
              "36690 100\n");
}

// §29.11: each `for` clones a fresh enumerator from the source frame; the
// clones borrow the source's reference, which the source drops once.
BEZEL_TEST(iterator_frame_string_retain, reenumerated_twice_spec_29_11_7798) {
    expectRun(syncProgram(withConcat("var it := Gen(", "); for var x in it do t := t + x; end; "
                                     "for var y in it do t := t + y; end;")),
              "21540 0\n");
}

// §29.7: a loop left early disposes a partially consumed enumerator.
BEZEL_TEST(iterator_frame_string_retain, break_after_first_value_spec_29_7_7798) {
    expectRun(syncProgram(withConcat("var it := Gen(", "); for var x in it do t := t + x; break; end;")),
              "3490 0\n");
}

// The iterator forwards its parameter to an inner iterator (base: SEGV).
BEZEL_TEST(iterator_frame_string_retain, parameter_forwarded_to_inner_iterator_spec_29_4_7798) {
    expectRun(syncProgram(withConcat("var it := Outer(", "); for var x in it do t := t + x; end;")),
              "11070 0\n");
}

// The frame returned out of the scope of the local it was built over: the
// local is released at `Mk`'s end, the frame still reads it.
BEZEL_TEST(iterator_frame_string_retain, frame_returned_past_local_scope_spec_29_4_7798) {
    expectRun(syncProgram("var it := Mk(i); for var x in it do t := t + x; end;"), "14370 0\n");
}

// §9.4 / §30.1: the `this` of an `extension string` iterator is held by the
// frame too (the frame returned past the receiver local's scope).
BEZEL_TEST(iterator_frame_string_retain, extension_string_receiver_spec_9_4_7798) {
    expectRun(syncProgram("var it := MkEach(i); for var x in it do t := t + x; end;"), "14370 0\n");
}

// Controls (clean on the base too): never enumerated, a literal argument, a
// named local that outlives the frame, and a named extension receiver
// enumerated twice. Each pins that the frame's +1 is dropped exactly once.
BEZEL_TEST(iterator_frame_string_retain, controls_release_once_spec_29_4_7798) {
    expectRun(syncProgram(withConcat("var it := Gen(", "); t := t + 1;")), "100 0\n");
    expectRun(syncProgram("var it := Gen(\"lit\"); for var x in it do t := t + x; end;"),
              "1200 0\n");
    expectRun(syncProgram(withConcat("var s := ", "; var it := Gen(s); for var x in it do t := t + x; end;")),
              "10770 0\n");
    expectRun(syncProgram(withConcat("var s := ", "; var it := s.Each(); for var x in it do t := t + x; end; "
                                     "for var y in it do t := t + y; end;")),
              "21540 0\n");
}

// §29.13 / §30.7: the async iterator, bound then `await for`, enumerated
// twice (each GetAsyncEnumerator clone takes its own reference), and inline.
BEZEL_TEST(iterator_frame_string_retain, async_iterator_string_param_spec_30_7_7798) {
    expectRun(asyncProgram(withConcat("var it := AIt(", "); await for var x in it do t := t + x; end;")),
              "10770 0\n");
    expectRun(asyncProgram(withConcat("var it := AIt(", "); await for var x in it do t := t + x; end; "
                                      "await for var y in it do t := t + y; end;")),
              "21540 0\n");
    expectRun(asyncProgram(withConcat("await for var x in AIt(", ") do t := t + x; end;")),
              "10770 0\n");
}

// The async iterator frame returned past the local's scope, as a parameter
// (enumerated twice) and as an `extension string` receiver.
BEZEL_TEST(iterator_frame_string_retain, async_frame_returned_past_local_scope_spec_30_7_7798) {
    expectRun(asyncProgram("var it := MkAIt(i); await for var x in it do t := t + x; end; "
                           "await for var y in it do t := t + y; end;"),
              "28740 0\n");
    expectRun(asyncProgram("var it := MkAEach(i); await for var x in it do t := t + x; end;"),
              "14370 0\n");
}

// §29.11: an explicit GetEnumerator() clone outlives its source frame. The
// enumerator is returned out of the helper whose temporary `Gen(s)` source
// (and local `s`) die at its end, and is driven by hand. The clone holds its
// own reference. 3 x 4690 + 300 = 14370 and 3 x 3490 + 300 = 10770.
BEZEL_TEST(iterator_frame_string_retain, getenumerator_clone_outlives_source_spec_29_11_7798) {
    expectRun(syncProgram("do using var e := En(i); while e.MoveNext() do t := t + e.Current; end; end;"),
              "14370 0\n");
    expectRun(syncProgram(withConcat("do using var e := EnArg(",
                                     "); while e.MoveNext() do t := t + e.Current; end; end;")),
              "10770 0\n");
}
