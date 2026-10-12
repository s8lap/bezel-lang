// Codegen / sema — fix #5858 (§30.15 + §30.14, v0.334): a `Channel<T>` whose
// element is a BUILT-IN standard-library class typed `Receive()` as `Task<T>`
// (Pattern A — a BORROW) instead of `OwningTask<T>` (Pattern B — an owned
// transfer), because the class test read `TypeRef::declaration->kind ==
// TypeDeclKind::Class` and a built-in class element carries no
// `ast::TypeDecl`.
//
// THE DEFECT, in two independent parts that both fell out of "built-in types
// have no user TypeDecl":
//
//   (1) BORROW-KIND INFERENCE. §30.15 (v0.334) verbatim: "`Receive()` is
//       asynchronous — `OwningTask<T>` for class-typed T (Pattern B: awaiting
//       transfers ownership of the item to the receiver), `Task<T>` otherwise."
//       The compiler's test was declaration-only, so it answered "value" for
//       every built-in class element — `Random`, `StringBuilder`, `Stopwatch`,
//       `OwningBuffer<double>`. §30.15's OWN canonical example is
//       `using var first := await inbox.Receive();`, which that inference made
//       a §30.5 Pattern A borrow flowing into a `using var` binding — the one
//       position §30.5's may-not-flow list names. Four banked `fail` rows
//       across four unrelated sections (l2_7_0005 §2.7, l3_1_0006 §3.1,
//       l4_14_0014 §4.14, l8_1_0003 §8.1) all carried that one diagnostic.
//
//   (2) SENDABILITY. §30.14 (v0.334) makes the stdlib section's Sendability
//       paragraph "the single normative carrier" of a compiler-known type's
//       classification; stdlib §8.1 states `Stopwatch` is sendable on the
//       copy-state ground. `Sendability.cpp` had entries for the two siblings
//       carrying that identical statement (`Random` stdlib §2.7, `StringBuilder`
//       stdlib §3.1) and none for `Stopwatch`, so `Channel<Stopwatch>` was
//       rejected at §30.14 before the borrow-kind question was ever reached.
//       `git log -S'Stopwatch' -- src/sema/Sendability.cpp` is empty — the
//       sibling the a7db94ef sendability sweep missed.
//
// ORACLES. Every row asserts the case's REAL OBSERVABLE OUTCOME (the value the
// program writes to stdout), never the mere absence of a diagnostic: a change
// that only silenced the §30.5 message would leave these programs producing
// nothing, or producing a restarted sequence, and would still fail here. Each
// accepted row runs on BOTH oracles — `runAot` and `runAotChecked` (the ASan /
// LSan net; this change moves a value from borrow-sourced to owned-transferred,
// so the sanitizer is load-bearing, not decorative).
//
// The two control rows pin the blast radius in BOTH directions: the §30.5
// Pattern A `using var` rejection must still fire for a genuine Pattern A task
// (a fix that simply disabled the check would pass the four rows above), and a
// VALUE-typed channel must stay Pattern A (multi-await permitted, §30.4 — a fix
// that flipped every channel to Pattern B would pass the four rows above too).

#include <string>

#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The unique text ONLY the §30.5 consumer-side `using var`-binding rejection
// carries (ExprTypecheck.cpp, the #4670 check). Distinct from the producer-side
// kind-1 rejection in async_pattern_a_task_factory_call_reject_4461_test.cpp —
// a different check in a different pass.
constexpr const char* kUsingVarBorrowDiagFragment =
    "the borrow produced by `await Task<T>` (Pattern A) may not flow to a ";

// Accept `src` and pin the program's stdout to `expected`, on BOTH oracles.
// `didNotCrash` is the runAot accept-side signal; the stdout compare is the
// row's real assertion.
void expectStdout(const std::string& src, const std::string& expected) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
    // ASan / LSan leg: same program, memory-checked. A `ran == false` here
    // means the sanitizer or the toolchain failed the leg — it is NOT a pass.
    JitResult rc = runAotChecked(src);
    BEZEL_EXPECT_TRUE(rc.ran);
    BEZEL_EXPECT_EQ(rc.stdoutText, expected);
}

// l2_7_0005 / stdlib §2.7 (v0.334): "`Random` is **sendable**, on the
// sendable form's copy-state ground. A moved instance continues its sequence
// in the receiving domain."
//
// The row's real content is the SEQUENCE, not the transfer: a moved Random
// resumes its stream rather than restarting. `Random(42)` is advanced once in
// the sending domain, then the same instance is compared against a fresh
// `Random(42)` advanced once in the receiving domain. Equality proves the
// moved instance continued (a restarted sequence would print "restarted"),
// and printing anything but "continued" fails the row.
constexpr const char* kRandomSrc =
    "module Conf; "
    "import System.Threading; "
    "public class Program "
    "  public static async method Main(): Task "
    "    using var inbox := new Channel<Random>(); "
    "    using var tx := inbox.NewSender(); "
    "    var r1 := new Random(42); "
    "    _ := r1.Next(); "
    "    tx.Send(move r1); "
    "    using var got := await inbox.Receive(); "
    "    using var r2 := new Random(42); "
    "    _ := r2.Next(); "
    "    if got.Next() = r2.Next() then "
    "      Console.WriteLine(\"continued\"); "
    "    else "
    "      Console.WriteLine(\"restarted\"); "
    "    end; "
    "  end Main; "
    "end Program; ";

// l3_1_0006 / stdlib §3.1 (v0.334): "`StringBuilder` is **sendable**, on the
// sendable form's copy-state ground. A moved builder keeps its contents."
//
// The observable is the CONTENT: `sb` is filled in the sending domain, moved
// across, and the receiving side reads back what was appended. A builder that
// lost its backing storage on the move would print the empty case.
constexpr const char* kStringBuilderSrc =
    "module Conf; "
    "import System.Threading; "
    "public class Program "
    "  public static async method Main(): Task "
    "    using var inbox := new Channel<StringBuilder>(); "
    "    using var tx := inbox.NewSender(); "
    "    var sb := new StringBuilder(); "
    "    sb.Append(\"kept\"); "
    "    tx.Send(move sb); "
    "    using var got := await inbox.Receive(); "
    "    Console.WriteLine(got.ToString()); "
    "  end Main; "
    "end Program; ";

// l4_14_0014 / stdlib §4.14 (v0.334): "`OwningBuffer<T>` is sendable iff its
// element type is a sendable class type or a sendable value type: its storage
// is part of the instance and crosses with it."
//
// The observable is the ELEMENT VALUE written before the move: `2.5` must
// still be readable through the received buffer. This is the row that pins
// `OwningBuffer<T>` (a generic built-in class — the heap-table membership path
// the two single-name rows above do not exercise).
constexpr const char* kOwningBufferSrc =
    "module Conf; "
    "import System.Threading; "
    "public class Program "
    "  public static async method Main(): Task "
    "    using var inbox := new Channel<OwningBuffer<double>>(); "
    "    using var tx := inbox.NewSender(); "
    "    var buf := new OwningBuffer<double>(2); "
    "    buf[0] := 2.5; "
    "    tx.Send(move buf); "
    "    using var got := await inbox.Receive(); "
    "    Console.WriteLine(got[0]); "
    "  end Main; "
    "end Program; ";

// l8_1_0003 / stdlib §8.1 (v0.334): "`Stopwatch` is **sendable**, on the
// sendable form's copy-state ground. A moved running stopwatch keeps
// measuring."
//
// This row exercises BOTH defects of this fix: the §30.14 sendability entry
// (without it the program is rejected at `new Channel<Stopwatch>()`) and the
// §30.15 Pattern B typing (without it `using var got := await …` is a §30.5
// borrow). The observable is that the moved stopwatch still reports itself
// running — a stopwatch lost or restarted across the move fails the row.
constexpr const char* kStopwatchSrc =
    "module Conf; "
    "import System.Diagnostics; "
    "import System.Threading; "
    "public class Program "
    "  public static async method Main(): Task "
    "    using var inbox := new Channel<Stopwatch>(); "
    "    using var tx := inbox.NewSender(); "
    "    var sw := Stopwatch.StartNew(); "
    "    tx.Send(move sw); "
    "    using var got := await inbox.Receive(); "
    "    Console.WriteLine(got.IsRunning); "
    "  end Main; "
    "end Program; ";

// CONTROL 1 — the §30.5 Pattern A `using var` rejection must STILL fire. The
// awaited task here is a genuine `Task<Widget>` (Pattern A: kind 1, a fresh
// owned result the task OWNS), so its await yields a BORROW and the
// may-not-flow list bars it from a `using var` binding. A fix that merely
// silenced the diagnostic would let this compile and the row would fail.
constexpr const char* kPatternAStillRejectedSrc =
    "module Conf; "
    "public class Widget5858 "
    "end Widget5858; "
    "public class C "
    "  public static async method ProduceAsync(): Task<Widget5858> "
    "    return new Widget5858(); "
    "  end ProduceAsync; "
    "  public static async method Main(): Task<int> "
    "    using var t := ProduceAsync(); "
    "    using var w := await t; "
    "    return 0; "
    "  end Main; "
    "end C; ";

// CONTROL 2 — a VALUE-typed channel stays Pattern A. §30.15's split is on
// class-typed T; `Channel<int>` must keep returning `Task<int>`, so the
// receive task is multi-await (§30.4) and the awaited value is a copy carrying
// no obligation. A fix that flipped EVERY channel to Pattern B would pass the
// four rows above and fail this one (the second await would be rejected).
constexpr const char* kValueChannelSrc =
    "module Conf; "
    "import System.Threading; "
    "public class C "
    "  public static async method Main(): Task "
    "    using var inbox := new Channel<int>(); "
    "    using var tx := inbox.NewSender(); "
    "    tx.Send(7); "
    "    using var t := inbox.Receive(); "
    "    var a := await t; "
    "    var b := await t; "
    "    Console.WriteLine(a + b); "
    "  end Main; "
    "end C; ";

}  // namespace

// ---- the four recovered conformance cases ---------------------------------

// §2.7 (stdlib §2.7): a moved `Random` continues its sequence.
BEZEL_TEST(channel_receive_builtin_class_element_5858,
           moved_random_continues_its_sequence_2_7) {
    expectStdout(kRandomSrc, "continued\n");
}

// §3.1 (stdlib §3.1): a moved `StringBuilder` keeps its contents.
BEZEL_TEST(channel_receive_builtin_class_element_5858,
           moved_string_builder_keeps_contents_3_1) {
    expectStdout(kStringBuilderSrc, "kept\n");
}

// §4.14 (stdlib §4.14): `OwningBuffer<double>` is sendable and its storage
// crosses with it.
BEZEL_TEST(channel_receive_builtin_class_element_5858,
           owning_buffer_storage_crosses_4_14) {
    expectStdout(kOwningBufferSrc, "2.5\n");
}

// §8.1 (stdlib §8.1): `Stopwatch` is sendable and a moved running stopwatch
// keeps running.
BEZEL_TEST(channel_receive_builtin_class_element_5858,
           moved_stopwatch_keeps_running_8_1) {
    expectStdout(kStopwatchSrc, "True\n");
}

// ---- controls: the fix must not widen past the class/value split ----------

// A genuine Pattern A `Task<T>` borrow still may not flow to a `using var`
// binding (§30.5 may-not-flow list, v0.334). Rejection with a REAL source
// location, and the specific diagnostic — not merely "failed to compile".
BEZEL_TEST(channel_receive_builtin_class_element_5858,
           pattern_a_using_var_bind_still_rejected_30_5) {
    JitResult r = runAot(kPatternAStillRejectedSrc);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kUsingVarBorrowDiagFragment));
}

// A value-typed channel keeps the Pattern A reading: `Receive()` is
// `Task<int>`, multi-await is permitted, and the value is a copy.
BEZEL_TEST(channel_receive_builtin_class_element_5858,
           value_element_channel_stays_pattern_a_30_15) {
    expectStdout(kValueChannelSrc, "14\n");
}