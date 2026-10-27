// The `await using var` COPY of a VALUE-LIKE CHOICE local — the awaited
// spelling of the shape #7316 fixed for the synchronous spelling (fix #7374).
//
// §28.12 (v0.345): "`await using var` takes §28.10's admission unchanged ...
// Over an owned right-hand side with no `DisposeAsync` to await - a value-type,
// runtime-managed or move-only right-hand side ... scope exit takes the
// synchronous disposal (or the no-op) and likewise awaits nothing."
//
// §28.10 (v0.345): "`using var` accepts a value-type or runtime-managed
// right-hand side. For ordinary value types and runtime-managed types, the
// disposal at scope end is a no-op (these types do not require disposal per
// §28.9), and the only effect is the reassignability restriction."
//
// A VALUE-LIKE CHOICE is a value type (§35.4), so its scope exit is the
// NO-OP — and the no-op is the reassignability restriction and NOTHING ELSE,
// so the awaited spelling's RUNTIME LIFETIME is the plain-`var` twin's. The
// runtime's own reference on the choice's payload string is not a disposal
// obligation (§28.9's exemption is from MANUAL disposal), so it must still be
// released.
//
// THE ORACLE. `runAotChecked` instruments the GENERATED code with
// AddressSanitizer/LeakSanitizer (`detect_leaks=1:exitcode=99`), so a leak
// flips `ran` to false. Every repro here LOOPS 101 times (the house's
// leak-oracle band is 50-500): under LSan's default options a ONE-SHOT leak
// repro is vacuous, because LSAN scans the stack for roots and the dead
// frame's stale payload pointer reads as a root.
//
// MEASURED on the base (e4a55c95), bezelc --sanitize=address, 101
// iterations: 3535 byte(s) leaked in 202 allocation(s). The relocation table
// for that shape is 1x release / 0x retain on the base arm: the copy is never
// ARMED, so the source's +1 is never discharged.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The choice local and the copy, in the spelling the row names.
constexpr const char* kAwaitUsingChoiceCopy = R"(
module M;
public choice Msg Note(text: string), Empty; end Msg;
public class Program
  public static async method Main(): Task
    for var i in 0..<101 do
      var a := Msg.Note("a" + "b");
      await using var b := a;
    end for;
    Console.WriteLine("END");
  end Main;
end Program;
)";

// The synchronous twin of the same program — the CONTROL. §28.10 makes the
// two spellings differ only in reassignability, and #7316 already fixed the
// synchronous spelling; the control is clean on BOTH arms and is what makes
// the awaited row a comparison rather than an absolute.
constexpr const char* kUsingChoiceCopy = R"(
module M;
public choice Msg Note(text: string), Empty; end Msg;
public class Program
  public static async method Main(): Task
    for var i in 0..<101 do
      var a := Msg.Note("a" + "b");
      using var b := a;
    end for;
    Console.WriteLine("END");
  end Main;
end Program;
)";

// The plain-`var` twin — the CONTROL whose runtime lifetime the awaited
// spelling must match.
constexpr const char* kPlainVarChoiceCopy = R"(
module M;
public choice Msg Note(text: string), Empty; end Msg;
public class Program
  public static async method Main(): Task
    for var i in 0..<101 do
      var a := Msg.Note("a" + "b");
      var b := a;
    end for;
    Console.WriteLine("END");
  end Main;
end Program;
)";

}  // namespace

// fix #7374 (§28.12 / §28.10, v0.345): the `await using var` COPY of a
// value-like choice local.
//
// Before the fix: 3535 byte(s) leaked in 202 allocations (101 iterations).
// After the fix: clean.
BEZEL_TEST(await_using_choice_copy,
           await_using_var_copy_releases_the_payload_at_scope_end_spec_28_12_7374) {
    bezel::test::JitResult r = runAotChecked(kAwaitUsingChoiceCopy);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "END\n");
}

// fix #7374 (§28.12 / §28.10, v0.345): the synchronous `using var` twin —
// the CONTROL. Clean on BOTH arms (#7316's fix).
BEZEL_TEST(await_using_choice_copy,
           using_var_copy_is_clean_control_spec_28_10_7374) {
    bezel::test::JitResult r = runAotChecked(kUsingChoiceCopy);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "END\n");
}

// fix #7374 (§28.12 / §28.10, v0.345): the plain-`var` twin — the CONTROL
// whose runtime lifetime the awaited spelling must match (§28.12: "`await
// using var` takes §28.10's admission unchanged"). Clean on BOTH arms.
BEZEL_TEST(await_using_choice_copy,
           plain_var_copy_is_clean_control_spec_28_10_7374) {
    bezel::test::JitResult r = runAotChecked(kPlainVarChoiceCopy);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "END\n");
}
