// fix #7014 / #7025 (follow-up, §27.6 / §28.2, v0.343): a method group
// formed at a position the ownership pre-scan did not walk — the INLINE
// ARGUMENT spelling.
//
// THE DEFECT. §27.6: "the instance is captured at formation — by reference
// for class- or interface-typed receivers". The capture is a BORROW of the
// receiver's block (§28.3), and §28.2: "An explicit `Dispose()` … performs
// **logical disposal only** — it sets the flag and runs the class's cleanup —
// and does not reclaim its receiver: the receiver's block stays live until
// the holder's release point".
//
// The borrow fact that decides whether the explicit `Dispose()` may reclaim
// eagerly lives on the receiver's SYMBOL, and the ownership pre-scan sets it
// as a WHOLE-BODY fact — the stamp has to be in place at the explicit
// `Dispose()` call site even for a group formed LATER in the body. The
// pre-scan recorded it for the LOCAL-INITIALIZER spelling (`var f := d.Dispose;`)
// and for the `const` spelling, but a group formed as a CALL ARGUMENT was
// never stamped at all.
//
// So for the shape below the explicit `Dispose()` reclaimed the receiver's
// block eagerly and NULLed its slot; the group formed afterwards captured the
// null, and its invocation SEGV'd. MEASURED on the tip:
//
//   ==...==ERROR: AddressSanitizer: SEGV on unknown address 0x000000000000
//   The signal is caused by a READ memory access.
//       #0 ... in disposethunk$Dispose
//       #1 ... in bz$M.C$Take
//
// §28.2's use-after-dispose regime covers this program — the compiler warns
// ("use of `d` after a dominating explicit Dispose() call"), and in checked
// mode the use raises DisposedAccessException — but the WARNING is not a
// licence to crash: §28.2's own sentence says the receiver's block stays
// live until the holder's release point, so the capture is a live block and
// the group's invocation is the already-set dispose flag's no-op.
//
// THE ROWS BELOW.
//
//   * the INLINE-ARGUMENT spelling, the group formed AFTER the dispose — the
//     shape that SEGV'd; the program must run and print what the base
//     printed;
//   * the CONTROL: the same spelling with the group formed BEFORE the
//     dispose — it already worked, and it must keep working;
//   * the CONTROL: the explicit dispose with NO group at all — the fix must
//     not change a program that forms no method group over the receiver.
//
// Each row is pinned with `runAotChecked`, so the AOT leg runs the program
// under AddressSanitizer/LeakSanitizer: the pre-fix SEGV fails the row, and
// a leak or a double free fails it too.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The class D with an observable destructor, and a static `Take` that
// invokes the function value it is handed.
constexpr char kDAndTake[] =
    "module M; "
    "class D "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"disposed\"); end destructor; "
    "end D; "
    "class C "
    "  public static method Take(f: () -> ()) f(); end Take; "
    "  public static method Main(): int ";

}  // namespace

// The INLINE-ARGUMENT spelling: the receiver is explicitly disposed, and the
// method group is formed afterwards, as a call argument.
//
// §28.2: the receiver's block stays live until the holder's release point,
// so the group captures a live block and its invocation is the already-set
// dispose flag's no-op ("a repeated call is the no-op").
//
// PRE-FIX: AddressSanitizer "SEGV on unknown address 0x000000000000" in
// `disposethunk$Dispose` — the capture was the nulled slot.
BEZEL_TEST(method_group_arg_receiver_7025,
           spec_27_6_inline_argument_group_formed_after_the_dispose_does_not_crash) {
    auto r = runAotChecked(
        std::string(kDAndTake) +
        "  var d := new D(); "
        "  d.Dispose(); "
        "  Take(d.Dispose); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The destructor runs EXACTLY once: the explicit `Dispose()` performs the
    // logical disposal (running the cleanup), and the group's later
    // invocation is the already-set flag's no-op (§28.2).
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n");
}

// The CONTROL: the same inline-argument spelling with the group formed
// BEFORE the dispose. It already behaved this way — the group is formed
// while the receiver is live — and it must keep behaving this way.
BEZEL_TEST(method_group_arg_receiver_7025,
           spec_27_6_inline_argument_group_formed_before_the_dispose_is_unchanged) {
    auto r = runAotChecked(
        std::string(kDAndTake) +
        "  var d := new D(); "
        "  Take(d.Dispose); "
        "  d.Dispose(); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n");
}

// The CONTROL: the explicit dispose with NO method group formed over the
// receiver. The borrow fact is a fact about a method group, so a program
// that forms none must keep the eager reclaim — the block is freed at the
// explicit dispose and the scope-end disposal is skipped.
//
// PRE-FIX: this row was already green; it is here so that a fix which
// stamped the borrow fact for EVERY receiver (not only for the ones a method
// group is formed over) would be caught.
BEZEL_TEST(method_group_arg_receiver_7025,
           spec_28_2_explicit_dispose_without_a_group_is_unchanged) {
    auto r = runAotChecked(
        std::string(kDAndTake) +
        "  var d := new D(); "
        "  d.Dispose(); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n");
}
