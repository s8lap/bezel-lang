// Codegen — issue #8753: a closure local passed to a store that keeps the
// pointer without a reference (`fs.Add(b)`, #5866) and THEN reassigned
// (`b := g;`) had its stored value released by the reassignment, so the
// caller's collection held a freed closure.
//
// THE RULE. §28.9 (v0.359, "Runtime-managed types"): a function value's
// lifetime is managed by the runtime without user involvement, so a function
// value a collection still holds stays callable. §27.5 (v0.359): a closure's
// captures live as long as the closure.
//
// ROOT CAUSE. `lowerClosureLocalReassign` releases the slot's old value at
// every reassignment, and the #8723 donor pass in
// `collectClosureReleaseEligible` judged a reassigned local as a whole: it
// passes "kept" on to its sources but never donates its own reference. The
// value `fs` keeps is `b`'s initial one, whose only reference is `b`'s, so
// `b := g;` freed it. The same happened to a value `b := lambda …;` stored,
// at the next reassignment or at `b`'s scope-end release.
//
// THE FIX. A reaching-definition analysis over the body, per reassigned
// closure local: a definition whose reference is the slot's own (the initial
// producer value or `b := <producer>`) and that a kept site has seen is
// donated by the slot. The reassignment that ends it does not release the
// old value (closureReassignKeepsOld_), and if one may reach the scope end
// kept the local has no scope-end or call-site release
// (closureReassignedKeptAtExit_). Joins are unions, so an imprecise path
// leaks rather than frees. What remains is the #5866 leak of the stored
// closure, so these rows use `runAotCheckedLenient` (AddressSanitizer on,
// LeakSanitizer off) with the full stdout compared.
//
// Every program loops 200 times and runs under generated-code
// AddressSanitizer.
//
// MEASURED (`bezelc --sanitize=address --link=system`, detect_leaks=1, on
// r30-c1 @ b80788db5 and on origin/main @ b279e8daf, so PRE-EXISTING, not
// introduced by the #8723 pass): every row below is heap-use-after-free with
// 0 lines on both; with the fix, 201 correct lines and the #5866 leak.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

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

// Iteration i calls the stored closure `x - y + i` twice:
// (1 - 2 + i) + (4 - 1 + i), after `prefix` (what Fill printed).
std::string expected200(const std::string& prefix = "") {
    std::string e;
    for (int i = 0; i < 200; ++i)
        e += prefix + std::to_string(2 + 2 * i) + "\n";
    e += "DONE\n";
    return e;
}

// `fillBody` follows the declarations of `g` (returns 0) and `b` (returns
// `x - y + k`); it must leave `b`'s initial closure in `fs[0]`.
std::string fillProgram(const std::string& fillBody) {
    return R"BZL(
module M;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => 0;
    var b: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
)BZL" + fillBody + R"BZL(
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL";
}

}  // namespace

// #8753's repro, as filed.
BEZEL_TEST(closure_local_kept_then_reassigned_8753,
           issue_repro_add_then_reassign_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    b := g;\n")),
        expected200());
}

// The reassignment runs on one path only; the other path reaches the scope
// end with the kept value still in `b`.
BEZEL_TEST(closure_local_kept_then_reassigned_8753,
           add_then_reassign_on_one_branch_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    if k mod 2 = 0 then\n"
        "      b := g;\n"
        "    end;\n")),
        expected200());
}

// Inside a loop: the value kept on the first iteration is overwritten on
// every iteration by a fresh lambda.
BEZEL_TEST(closure_local_kept_then_reassigned_8753,
           add_in_loop_then_reassign_each_iteration_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    for var j in 0..<3 do\n"
        "      if j = 0 then\n"
        "        fs.Add(b);\n"
        "      end;\n"
        "      b := lambda(x: int, y: int) => j;\n"
        "    end for;\n")),
        expected200());
}

// Loop-carried: the value `b := lambda …;` stores on iteration 0 is kept,
// and iteration 1's reassignment (across the back edge) ends it.
BEZEL_TEST(closure_local_kept_then_reassigned_8753,
           loop_carried_reassign_after_add_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    for var j in 0..<2 do\n"
        "      b := lambda(x: int, y: int) => x - y + k;\n"
        "      fs.Add(b);\n"
        "    end for;\n")),
        expected200());
}

// Reassign-to-self: `b := b;` retains the value and releases the old one,
// the same value, which the scope-end release then freed.
BEZEL_TEST(closure_local_kept_then_reassigned_8753,
           add_then_reassign_to_self_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    b := b;\n")),
        expected200());
}

// A call through the reassigned local (the #4422 call-site release reads the
// reassignment record).
BEZEL_TEST(closure_local_kept_then_reassigned_8753,
           add_then_reassign_then_call_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    b := g;\n"
        "    Console.Write(b(1, 1));\n")),
        expected200("0"));
}

// An early `return` after the reassignment on one path, a fresh lambda on
// the other.
BEZEL_TEST(closure_local_kept_then_reassigned_8753,
           add_then_reassign_then_early_return_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    if k mod 3 = 0 then\n"
        "      b := g;\n"
        "      return;\n"
        "    end;\n"
        "    b := lambda(x: int, y: int) => 7;\n")),
        expected200());
}

// `b`'s initial value is kept, then `g`'s value passes through `b` into a
// second kept store: `g` donates (#8723) and `b`'s initial value survives.
BEZEL_TEST(closure_local_kept_then_reassigned_8753,
           add_reassign_from_local_then_add_again_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    b := g;\n"
        "    fs.Add(b);\n")),
        expected200());
}

// Inside `try … catch … finally`: the store and reassignment in the body,
// further reassignments in the catch arm and the finally clause (§31.2).
BEZEL_TEST(closure_local_kept_then_reassigned_8753,
           add_then_reassign_in_try_finally_spec_31_2) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    try\n"
        "      fs.Add(b);\n"
        "      b := g;\n"
        "    catch\n"
        "      var e: Exception => do b := g; end\n"
        "    finally\n"
        "      b := g;\n"
        "    end try;\n")),
        expected200());
}

// Reassignment in the arms of a `case` statement (§19).
BEZEL_TEST(closure_local_kept_then_reassigned_8753,
           add_then_reassign_in_case_arms_spec_19_1) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(b);\n"
        "    case k mod 3 of\n"
        "      0 => do b := g; end,\n"
        "      _ => do b := lambda(x: int, y: int) => 3; end\n"
        "    end case;\n")),
        expected200());
}
