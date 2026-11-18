// Codegen — issue #8750: a closure local used inside an interpolation hole
// `$"{…}"` was invisible to the closure escape analysis, so a store made from
// inside the hole kept a function value the frame then freed.
//
// THE RULE. §18.5 (v0.359): an interpolation hole holds an ordinary
// expression, evaluated like any other. §28.9 (v0.359, "Runtime-managed
// types"): a function value's lifetime is managed by the runtime without user
// involvement, so a function value a collection still holds stays callable.
// (§28.9 leaves the lifecycle mechanism to the implementation; the citation
// is for the observable guarantee.)
//
// ROOT CAUSE. `ClosureEscapeWalker::expr` (LlvmBackend_Setup.cpp) had no arm
// for `ast::InterpolatedStringExpr`: its parts fell through to the "leaf
// literals contain no identifiers" end. A closure local named inside a hole
// was therefore neither a read nor an escape. The local kept its scope-end
// release (and the free of its capture cells) although `fs.Add(r.F)`, run
// from inside the hole, had stored its closure in the caller's array. The
// #8717 forwarded-bind pass and #8743's record / identity holders consult the
// same walker, so `var b := n ?? g;`, `var rb := new RB(g);` and
// `var g1 := C.Id<Fn>(n ?? g);` used inside a hole fell through the same gap.
//
// THE FIX. One arm that walks every hole expression in an escaping position
// (safeRead = false), as an argument of an unknown call would be. A call OF
// the local inside a hole (`$"{g(5, 2)}"`, `$"{rb.F(5, 2)}"`) is still only a
// call: the CallExpr arm decides that, not the position. A hole that is the
// bare name (`$"{g}"`) only renders the value and stays a read; that boundary
// is pinned by nullable_function_render_7564.nullable_function_interpolation_
// loop_100, which leaks under an arm that treats the bare name as an escape.
//
// Every program loops 200 times and runs under generated-code
// AddressSanitizer. The store itself takes no reference (#5866), so what the
// fix leaves is a leak of the kept closure, and the oracle for the kept rows
// is `runAotCheckedLenient` (AddressSanitizer on, LeakSanitizer off) with the
// full stdout compared. The control runs under `runAotChecked` (both on).
//
// MEASURED (`bezelc --sanitize=address`, ASAN_OPTIONS=detect_leaks=1):
//   row                                                     r30-c1   551624deb   with the fix
//   issue_repro_record_built_in_hole_spec_18_5              UAF      UAF         leak
//   coalesce_bound_local_boxed_in_hole_spec_7_7             UAF      UAF         leak
//   identity_call_in_hole_spec_18_5                         UAF      UAF         leak
//   record_holder_local_passed_in_hole_spec_18_5            leak     UAF         leak
//   bare_record_holder_local_passed_in_hole_spec_18_5       leak     UAF         leak
//   identity_bound_local_boxed_in_hole_spec_18_5            leak     UAF         leak
//   control_call_only_in_hole_stays_released_spec_18_5      leak     clean       clean
// (UAF = heap-use-after-free in main when the stored closure is called.)

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
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

// 200 calls of `Fill(fs, i)`; each stored closure computes `x - y + k`, so
// iteration i prints 5 - 2 + i.
std::string expected200() {
    std::string e;
    for (int i = 0; i < 200; ++i) e += std::to_string(3 + i) + "\n";
    e += "DONE\n";
    return e;
}

// `fillBody` is the body of `Fill(fs, k)`; it must leave exactly one closure
// in `fs`.
std::string program(const std::string& fillBody) {
    return R"BZL(
module M;
type Fn := (int, int) -> int;
public record RB(F: (int, int) -> int);
public class C
  public static method Id<T>(x: T): T => x;
  public static method KeepR(r: RB, fs: Array<(int, int) -> int>): int
    fs.Add(r.F);
    return 1;
  end KeepR;
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var n: ((int, int) -> int)? := null;
)BZL" + fillBody + R"BZL(
    if s.Length = 0 then Console.WriteLine(s); end;
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      Console.WriteLine(fs[0](5, 2));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL";
}

}  // namespace

// #8750's repro, as filed: the record is built from `g` and stored from
// inside the hole. 200 * (5 - 2 + k - k) = 600.
BEZEL_TEST(closure_escape_interpolation_hole_8750,
           issue_repro_record_built_in_hole_spec_18_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
import System;
import System.Collections;
public record RB(F: (int, int) -> int);
public class C
  public static method KeepR(r: RB, fs: Array<(int, int) -> int>): int
    fs.Add(r.F);
    return 1;
  end KeepR;
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k - k;
    var s := $"{C.KeepR(new RB(g), fs)}";
    if s.Length = 0 then Console.WriteLine(s); end;
  end Fill;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      for var f in fs do t := t + f(5, 2); end for;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL"), std::string("600\n"));
}

// The #8717 forwarded bind `var b := n ?? g;` (§7.7): `b`'s use inside the
// hole decides `g`'s release.
BEZEL_TEST(closure_escape_interpolation_hole_8750,
           coalesce_bound_local_boxed_in_hole_spec_7_7) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var b := n ?? g;\n"
        "    var s := $\"{C.KeepR(new RB(b), fs)}\";\n")),
        expected200());
}

// The identity call sits inside the hole with the record construction.
BEZEL_TEST(closure_escape_interpolation_hole_8750,
           identity_call_in_hole_spec_18_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var s := $\"{C.KeepR(new RB(C.Id<Fn>(n ?? g)), fs)}\";\n")),
        expected200());
}

// #8743's record holder: `rb` is passed on from inside the hole.
BEZEL_TEST(closure_escape_interpolation_hole_8750,
           record_holder_local_passed_in_hole_spec_18_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var rb := new RB(n ?? g);\n"
        "    var s := $\"{C.KeepR(rb, fs)}\";\n")),
        expected200());
}

// The same holder built from the bare local.
BEZEL_TEST(closure_escape_interpolation_hole_8750,
           bare_record_holder_local_passed_in_hole_spec_18_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var rb := new RB(g);\n"
        "    var s := $\"{C.KeepR(rb, fs)}\";\n")),
        expected200());
}

// #8743's identity-call holder: `g1` is boxed into the record inside the
// hole.
BEZEL_TEST(closure_escape_interpolation_hole_8750,
           identity_bound_local_boxed_in_hole_spec_18_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(program(
        "    var g1 := C.Id<Fn>(n ?? g);\n"
        "    var s := $\"{C.KeepR(new RB(g1), fs)}\";\n")),
        expected200());
}

// BASE-GREEN CONTROL (lane contract §7; green on 551624deb, red under the
// plausible wrong fix "every local named in a hole escapes"): a hole that
// only CALLS the closure - directly and through the record holder - stores
// nothing, so `g` is still released with its frame and the run is leak-free.
// 200 * ((5 - 2 + i) + (5 - 2 + i)) summed: 2 * (600 + 19900) = 41000.
BEZEL_TEST(closure_escape_interpolation_hole_8750,
           control_call_only_in_hole_stays_released_spec_18_5) {
    auto r = runAotChecked(R"BZL(
module M;
public record RB(F: (int, int) -> int);
public class C
  public static method Sum(k: int): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var n: ((int, int) -> int)? := null;
    var rb := new RB(n ?? g);
    var s := $"{g(5, 2)}|{rb.F(5, 2)}";
    if s.Length = 0 then return 0; end;
    return g(5, 2) + rb.F(5, 2);
  end Sum;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do
      t := t + Sum(i);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("41000\n"));
}
