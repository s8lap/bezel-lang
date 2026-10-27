// The `using var` COPY of a RUNTIME-MANAGED value (a string) — the copy's own
// reference, and the SOURCE's, are discharged at scope end (fix #7388).
//
// §28.10 (v0.345): "`using var` accepts a value-type or runtime-managed
// right-hand side. For ordinary value types and runtime-managed types, the
// disposal at scope end is a no-op (these types do not require disposal per
// §28.9), and the only effect is the reassignability restriction."
//
// §28.9's exemption is from MANUAL disposal. It is NOT a licence to skip
// releasing the RUNTIME's own reference: §28.10's next words are "and the only
// effect is the reassignability restriction", so the `using var` spelling of a
// runtime-managed value must have the SAME RUNTIME LIFETIME as its plain-`var`
// twin.
//
// THE ORACLE. `runAotChecked` instruments the GENERATED code with
// AddressSanitizer/LeakSanitizer (`detect_leaks=1:exitcode=99`), so a leak
// flips `ran` to false and the exit code to 99. Every repro here LOOPS (the
// house's leak-oracle band is 50-500): under LSan's default options a
// ONE-SHOT repro is vacuous, because LSAN scans the stack for roots and a
// dead frame's stale payload pointer reads as a root.
//
// MEASURED on the base (e4a55c95), bezelc --sanitize=address, loop x60:
//
//   var src := o.ToString(); using var x := src;  -> 2040 B / 120 allocs
//   var src := o.ToString(); var         x := src;  -> CLEAN
//   const src := o.ToString(); using var x := src; -> 2040 B / 120 allocs
//   var src := o.ToString(); await using var x := src; -> 2040 B / 120 allocs
//
// THE MECHANISM, measured: the leak is in the SOURCE, not in the copy. The
// copy was never classified (the candidate scan refused the `using var`
// spelling), so it took NO retain at bind; and because it took no retain, the
// escape walk's copy relaxation did not fire for it, so the walk tainted the
// SOURCE as escaping — and the source's producer +1 was then never
// discharged.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The loop body, shared by the four rows: the SOURCE is a +1 string producer,
// and the COPY is spelled by the row's `binding`.
std::string copyProgram(const std::string& binding, bool isAsync) {
    std::string src = R"(
module M;
public class Program
  public static %s method Main(): %s
    for var i in 0..<60 do
      var o := 1;
      var src := o.ToString();
      %s
    end for;
    Console.WriteLine("END");
    %s
  end Main;
end Program;
)";
    std::string out;
    const std::string a = isAsync ? "async" : "";
    const std::string b = isAsync ? "Task" : "int";
    // The four substitutions, in order. An `async` method returns `Task`, so
    // it has no `return 0;` — the two spellings differ only there.
    const std::string values[4] = {a, b, binding, isAsync ? "" : "return 0;"};
    std::size_t i = 0;
    int slot = 0;
    while (i < src.size()) {
        if (src.compare(i, 2, "%s") == 0) {
            out += values[slot++];
            i += 2;
        } else {
            out += src[i++];
        }
    }
    return out;
}

}  // namespace

// fix #7388 (§28.9 / §28.10, v0.345): the `using var` COPY of a string local.
//
// Before the fix: 2040 B leaked in 120 allocations (60 iterations x 34 B / 2
// allocations). After the fix: clean.
BEZEL_TEST(using_var_string_copy, using_var_copy_of_string_local_does_not_leak_spec_28_9_7388) {
    bezel::test::JitResult r = runAotChecked(
        copyProgram("using var x := src;", /*isAsync=*/false));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "END\n");
}

// fix #7388 (§28.9 / §28.10, v0.345): the plain-`var` twin — the CONTROL.
// §28.10 makes the two spellings differ ONLY in reassignability, so the twin
// is the reference lifetime the `using var` spelling must match. It is clean
// on BOTH arms of the fix, and it is what makes the `using var` row's
// expectation a comparison rather than an absolute.
BEZEL_TEST(using_var_string_copy, plain_var_copy_of_string_local_is_clean_control_spec_28_9_7388) {
    bezel::test::JitResult r = runAotChecked(
        copyProgram("var x := src;", /*isAsync=*/false));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "END\n");
}

// fix #7388 (§28.9 / §28.10, v0.345): the `await using var` spelling.
// §28.12 (v0.345): "`await using var` takes §28.10's admission unchanged", so
// the awaited spelling has §28.10's RUNTIME LIFETIME. Before the fix: 2040 B
// leaked in 120 allocations.
BEZEL_TEST(using_var_string_copy, await_using_copy_of_string_local_does_not_leak_spec_28_10_7388) {
    bezel::test::JitResult r = runAotChecked(
        copyProgram("await using var x := src;", /*isAsync=*/true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "END\n");
}

// fix #7388 (§28.9 / §28.10, v0.345): the §21.1 `const` SOURCE. The `const`
// is a runtime binding with no disposal obligation of its own (§21.1), so the
// copy is the only place a reference could be discharged — and before the fix
// it was not: 2040 B leaked in 120 allocations.
BEZEL_TEST(using_var_string_copy, using_var_copy_of_const_string_source_does_not_leak_spec_28_9_7388) {
    bezel::test::JitResult r = runAotChecked(R"(
module M;
public class Program
  public static method Main(): int
    for var i in 0..<60 do
      var o := 1;
      const src := o.ToString();
      using var x := src;
    end for;
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "END\n");
}
