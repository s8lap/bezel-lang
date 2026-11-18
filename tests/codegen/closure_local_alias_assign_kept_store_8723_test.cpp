// Codegen — issue #8723: a closure local assigned into another closure local
// (`b := g;`) whose value then reached a store that keeps the pointer without
// a reference (`fs.Add(b)`, #5866) was released twice at scope end, so the
// caller's collection held a freed closure.
//
// THE RULE. §28.9 (v0.359, "Runtime-managed types"): a function value's
// lifetime is managed by the runtime without user involvement, so a function
// value a collection (or a caller) still holds stays callable. §27.5
// (v0.359): a closure's captures live as long as the closure. (§28.9 leaves
// the lifecycle mechanism to the implementation; the citation is for the
// observable guarantee.)
//
// ROOT CAUSE. `collectClosureReleaseEligible` (LlvmBackend_Setup.cpp) judged
// each closure local alone. `b := g;` retains `g`'s value for `b`
// (`lowerClosureLocalReassign`), and the #4473 arm clears `g`'s escape for
// that assignment, so `g` kept its scope-end release; `b`'s only escape,
// `fs.Add(b)`, takes no reference, and `b` kept its release as well. Two
// releases against one retain: the count reached zero while `fs` held the
// pointer. The `return a;` spelling from the issue comment (sync, async
// `Task<Fn>`, `Object`) lost the value the other way: `a`'s return is a
// transfer, but `g` stayed in closureReleaseEligible_, so its capture cells
// were freed with the frame while the returned closure still read them.
//
// THE FIX. A pass at the end of `collectClosureReleaseEligible` follows each
// value along its assignment and owned-alias edges. A local whose value
// reaches a site that keeps it without a reference donates its own reference
// to that site: it leaves both scope-end release sets. A local whose value
// reaches a local that leaves the frame (returned, stored with a reference)
// keeps its release but not the free of its capture cells. What the fix
// leaves for a kept store is the #5866 leak of the stored closure, so the
// oracle for those rows is `runAotCheckedLenient` (AddressSanitizer on,
// LeakSanitizer off) with the full stdout compared. The control runs under
// `runAotChecked` (both on).
//
// Every program loops 200 times and runs under generated-code
// AddressSanitizer.
//
// MEASURED (`bezelc --sanitize=address --link=system`, detect_leaks=1):
//   row                                              r30-c1 b279e8daf   with the fix
//   issue_repro_alias_assign_then_add_spec_28_9      UAF, 0 lines        leak, 201 lines
//   alias_chain_then_add_spec_28_9                   UAF, 0 lines        leak, 201 lines
//   coalesce_assign_then_add_spec_7_7                UAF, 0 lines        leak, 201 lines
//   alias_assign_then_return_spec_28_9               UAF, 0 lines        leak, 201 lines
//   alias_assign_then_return_task_spec_30_5          UAF, 0 lines        leak, 201 lines
//   alias_assign_then_return_object_spec_12_7        UAF, 0 lines        leak, 201 lines
//   control_alias_assign_confined_stays_released...  clean               clean
// (UAF = heap-use-after-free in main when the stored or returned closure is
// called. The control leaked under a draft of the fix that missed the
// assignment's own retain, so it pins the boundary.)

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

// Iteration i calls the closure `x - y + i` twice: (1 - 2 + i) + (4 - 1 + i).
std::string expected200() {
    std::string e;
    for (int i = 0; i < 200; ++i) e += std::to_string(2 + 2 * i) + "\n";
    e += "DONE\n";
    return e;
}

// `fillBody` is the body of `Fill(fs, k)` after `g`'s declaration; it must
// leave exactly one closure, `g`'s, in `fs`.
std::string fillProgram(const std::string& fillBody) {
    return R"BZL(
module M;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
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

// #8723's repro, as filed.
BEZEL_TEST(closure_local_alias_assign_kept_store_8723,
           issue_repro_alias_assign_then_add_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    var b: (int, int) -> int := lambda(x: int, y: int) => 0;\n"
        "    b := g;\n"
        "    fs.Add(b);\n")),
        expected200());
}

// The value passes through two assignments before the store: `g` donates,
// `a` and `b` only pass it on.
BEZEL_TEST(closure_local_alias_assign_kept_store_8723,
           alias_chain_then_add_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    var a: (int, int) -> int := lambda(x: int, y: int) => 0;\n"
        "    var b: (int, int) -> int := lambda(x: int, y: int) => 1;\n"
        "    a := g;\n"
        "    b := a;\n"
        "    fs.Add(b);\n")),
        expected200());
}

// #8717's assignment spelling (§7.7): `n ?? g` with `n` null forwards `g`.
// The issue names it as the remaining use-after-free of that shape.
BEZEL_TEST(closure_local_alias_assign_kept_store_8723,
           coalesce_assign_then_add_spec_7_7) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    var n: ((int, int) -> int)? := null;\n"
        "    var b: (int, int) -> int := lambda(x: int, y: int) => 0;\n"
        "    b := n ?? g;\n"
        "    fs.Add(b);\n")),
        expected200());
}

// The issue comment's `return` spelling, sync: the caller calls the value
// `Mk` returns.
BEZEL_TEST(closure_local_alias_assign_kept_store_8723,
           alias_assign_then_return_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Mk(k: int): (int, int) -> int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := lambda(x: int, y: int) => 0;
    a := g;
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var f: (int, int) -> int := Mk(i);
      Console.WriteLine(f(1, 2) + f(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), expected200());
}

// The same return from an async body (§30.5): the value reaches the caller
// through the `Task<Fn>`.
BEZEL_TEST(closure_local_alias_assign_kept_store_8723,
           alias_assign_then_return_task_spec_30_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
type Fn := (int, int) -> int;
public class C
  public static async method Mk(k: int): Task<Fn>
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var a: Fn := lambda(x: int, y: int) => 0;
    a := g;
    return a;
  end Mk;
  public static async method Main(): Task<int>
    for var i in 0..<200 do
      var f := await Mk(i);
      Console.WriteLine(f(1, 2) + f(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), expected200());
}

// The same return at an `Object` result (§12.7): the caller recovers the
// function value with `as`.
BEZEL_TEST(closure_local_alias_assign_kept_store_8723,
           alias_assign_then_return_object_spec_12_7) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
type Fn := (int, int) -> int;
public class C
  public static method Mk(k: int): Object
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var a: Fn := lambda(x: int, y: int) => 0;
    a := g;
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var o := Mk(i);
      var f := o as Fn;
      Console.WriteLine(f!(1, 2) + f!(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), expected200());
}

// BASE-GREEN CONTROL (lane contract §7; green on b279e8daf, red under the
// plausible wrong fix "an assignment into a closure local does not take a
// reference", which measured a 200-closure leak): when the assigned-into
// local is only called, both locals keep their scope-end release and the run
// is leak-free. Run(i) = 1 - 2 + i; summed over 0..199: 19700.
BEZEL_TEST(closure_local_alias_assign_kept_store_8723,
           control_alias_assign_confined_stays_released_spec_28_9) {
    auto r = runAotChecked(R"BZL(
module M;
public class C
  public static method Run(k: int): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var b: (int, int) -> int := lambda(x: int, y: int) => 0;
    b := g;
    return b(1, 2);
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      s := s + Run(i);
    end for;
    Console.WriteLine(s);
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("19700\nDONE\n"));
}
