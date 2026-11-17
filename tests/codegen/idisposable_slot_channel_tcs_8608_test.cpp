// Spec-derived regression for issue #8608: a `Channel<T>`, a
// `ChannelSender<T>` and a completion source (`TaskCompletionSource<T>`, the
// non-generic `TaskCompletionSource`, `OwningTaskCompletionSource<T>`) stored
// in an owned `IDisposable` slot were disposed through a vtable they did not
// have — SEGV.
//
// THE RULE
// ========
// Stdlib §2.10 (v0.357): "Every class implicitly implements `IDisposable`".
// Stdlib §6.6 declares `public class TaskCompletionSource<T>`, `public class
// OwningTaskCompletionSource<T> where T: class` and `public class
// TaskCompletionSource`; stdlib §6.9 declares `Channel<T>` and
// `ChannelSender<T>` as classes. So each converts to `IDisposable` and may be
// held in a slot of that type.
//
// The three slot kinds, and the sentence that disposes each:
//
//   * an `owned d: IDisposable` PARAMETER — §28.3 K3.14 (v0.357): "An owned
//     value that reaches its scope's end undischarged and not moved-from is
//     disposed there implicitly";
//   * an `owned F: IDisposable?` FIELD — §28.4 step 2 (v0.357): the generated
//     `Dispose()` "Disposes all owned-bearing fields in reverse declaration
//     order"; §28.5: the modifier "**Auto-disposes** the field when the
//     containing object is disposed";
//   * an `OwningArray<IDisposable>` ELEMENT — §28.11 (v0.357): the
//     collection's `Dispose()` "**disposes** every live element".
//
// What disposing each value does is its own type's rule:
//
//   * a channel — stdlib §6.9: "Disposing the `Channel<T>` closes it
//     immediately and disposes every undelivered class-typed item in reverse
//     insertion order; subsequent `Send` raises `ChannelClosedException`";
//   * a sender — stdlib §6.9: "Disposing the last sender closes the channel";
//   * a completion source — stdlib §6.6: its destructor "disposes the
//     produced task, faulting it first if the source is disposed before
//     completion (spec §30.11)"; an `OwningTaskCompletionSource<T>` whose
//     result was never taken disposes it (§28.3).
//
// THE DEFECT (measured on r30-c1 = b07571e52, spec v0.357, ASan)
// ==============================================================
// All three slots discharge by the class protocol: load word 0 as a vtable,
// call slot 2, reclaim the block.  None of these had a word-0 vtable: a
// channel's word 0 was its mutex (SEGV at 0x10), a sender's its core pointer
// and a completion source's its inner task (a call through pc 0).  The
// parameter rows crashed on r30-c1 before #7987; the field and element rows
// leaked before #7987 / #8533 and crash since.
//
// THE FIX
// =======
// The program's `Channel<T>` is now a small malloc'd handle (vtable word,
// core pointer) in front of the refcounted core senders keep alive (fix
// #634) — the split #8000 gave Semaphore — and the sender and completion
// source structs open with a vtable word. Slot 2 of each runs the typed
// release short of freeing the block: the channel's close-and-drain, the
// sender's give-back (the last one closes the channel), and the completion
// source's four `emitTcsRelease` steps.
//
// Every row asserts stdout — the destructor count of what the value owned,
// or the observable effect of its disposal — so a remedy that stopped
// crashing by NOT disposing fails too, and runs under the `runAotChecked`
// ASan/LSan oracle (no leak, no double free, no wild read).  Each row
// SEGV'd on r30-c1.

#include <cstdio>
#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

constexpr int kIters = 100;  // a one-shot repro is a vacuous leak oracle

// The declarations every row shares; `body` is the loop body, which may use
// the loop counter `i`; `methods` are extra members of `C`.
std::string prog(const std::string& body, const std::string& methods = "") {
    return
        "module M;\n"
        "import System.Collections.Generic;\n"
        "import System.Threading;\n"
        "public sealed class Res\n"
        "  public constructor() end constructor;\n"
        "  destructor\n"
        "    Console.WriteLine(\"RD\");\n"
        "  end destructor;\n"
        "end Res;\n"
        "public class G\n"
        "  public static method Take(owned d: IDisposable)\n"
        "  end Take;\n"
        "end G;\n"
        "public class H\n"
        "  public owned F: IDisposable? := null;\n"
        "  public constructor() end constructor;\n"
        "end H;\n"
        "public class C\n" +
        methods +
        "  public static method Main(): int\n"
        "    var i := 0;\n"
        "    while i < " + std::to_string(kIters) + " do\n" +
        body +
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"end\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The three slot kinds.  `setup` declares an owned local `c`; each form hands
// it to the slot, which then owns it and disposes it.
std::string atParameter(const std::string& setup) {
    return prog(setup + "      G.Take(move c);\n");
}
std::string atField(const std::string& setup) {
    return prog(setup +
                "      var h := new H();\n"
                "      h.F := c;\n");
}
std::string atElement(const std::string& setup) {
    return prog(setup +
                "      var a := new OwningArray<IDisposable>();\n"
                "      a.Add(move c);\n");
}

// `perIter` repeated once per loop iteration, then the `end` marker.
std::string expected(const std::string& perIter) {
    std::string out;
    for (int i = 0; i < kIters; ++i) out += perIter;
    return out + "end\n";
}

// The row must RUN, must be ASan/LSan-clean, and must print exactly `out`.
// There is deliberately NO absorbing `if (skipped) return;` guard: a
// toolchain SKIP must surface as a FAILURE, never as a silent pass.
void expectCleanUnderAsan(const std::string& program, const std::string& out) {
    JitResult r = runAotChecked(program);
    if (!r.ran || r.exitCode != 0)
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "    [8608 row] %.400s\n", d.message.c_str());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// ---- the values ----------------------------------------------------------

// Stdlib §6.9: a channel holding an undelivered class item; disposing the
// channel disposes the item (one `RD`). The sender `s` outlives the channel
// in the parameter row (released at the iteration's scope end).
const char* const kChannel =
    "      var c := new Channel<Res>();\n"
    "      var s := c.NewSender();\n"
    "      s.Send(new Res());\n";
// A value channel: nothing to run; LSan is the oracle for the core and the
// handle.
const char* const kChannelInt =
    "      var c := new Channel<int>();\n";
// Stdlib §6.9: the only sender of a channel with an undelivered item; the
// channel (a typed local) disposes the item at the iteration's end (one
// `RD`), whichever of the two goes first.
const char* const kSender =
    "      var ch := new Channel<Res>();\n"
    "      var c := ch.NewSender();\n"
    "      c.Send(new Res());\n";
// Stdlib §6.6: a pending source — disposal faults its task first.
const char* const kTcsPending =
    "      var c := new TaskCompletionSource<int>();\n";
// Stdlib §6.6: the non-generic spelling, completed before its disposal.
const char* const kTcsVoidSet =
    "      var c := new TaskCompletionSource();\n"
    "      c.SetResult();\n";
// Stdlib §6.6 fault-path ownership: the task owns its fault exception and
// disposes it exactly once, at task disposal (LSan is the oracle).
const char* const kTcsFaulted =
    "      var c := new TaskCompletionSource<int>();\n"
    "      c.SetException(new InvalidOperationException(\"boom\"));\n";
// §28.3 / stdlib §6.6: an unconsumed owned result is disposed with the
// source (one `RD`).
const char* const kOwningTcs =
    "      var c := new OwningTaskCompletionSource<Res>();\n"
    "      c.SetResult(new Res());\n";

}  // namespace

// ---- Channel<T> -----------------------------------------------------------

BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_channel_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kChannel), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_channel_at_owned_field) {
    expectCleanUnderAsan(atField(kChannel), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_channel_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kChannel), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_value_channel_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kChannelInt), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_value_channel_at_owned_field) {
    expectCleanUnderAsan(atField(kChannelInt), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_value_channel_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kChannelInt), expected(""));
}

// A sender that OUTLIVES its channel: the channel is disposed through the
// slot first, so the core must stay alive for the sender (fix #634).  Stdlib
// §6.9 *Closing*: after the channel's disposal "subsequent `Send` raises
// `ChannelClosedException`", and on a closed channel "subsequent
// `Send`/`SendAsync` raise `ChannelClosedException` (disposing the offered
// item first)" — so `RD` (the offered item) precedes `CCE`.
namespace {
std::string senderOutlives(const std::string& discharge) {
    return prog(
        "      var ch := new Channel<Res>();\n"
        "      var s := ch.NewSender();\n" +
        discharge +
        "      try\n"
        "        s.Send(new Res());\n"
        "      catch\n"
        "        ChannelClosedException => do Console.WriteLine(\"CCE\"); end\n"
        "      end try;\n");
}
}  // namespace
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_sender_outlives_channel_disposed_at_owned_parameter) {
    expectCleanUnderAsan(senderOutlives("      G.Take(move ch);\n"),
                         expected("RD\nCCE\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_sender_outlives_channel_disposed_at_owned_field) {
    expectCleanUnderAsan(senderOutlives("      var h := new H();\n"
                                        "      h.F := ch;\n"
                                        "      h.Dispose();\n"),
                         expected("RD\nCCE\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_sender_outlives_channel_disposed_at_owning_array_element) {
    expectCleanUnderAsan(senderOutlives("      var a := new OwningArray<IDisposable>();\n"
                                        "      a.Add(move ch);\n"
                                        "      a.Dispose();\n"),
                         expected("RD\nCCE\n"));
}

// ---- ChannelSender<T> -----------------------------------------------------

BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_sender_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kSender), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_sender_at_owned_field) {
    expectCleanUnderAsan(atField(kSender), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_sender_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kSender), expected("RD\n"));
}

// Stdlib §6.9: "Disposing the last sender closes the channel; buffered items
// remain receivable". The slot's discharge is that disposal, so the channel
// reads closed straight after it, and the buffered item is still there.
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_last_sender_at_owned_field_closes_the_channel) {
    expectCleanUnderAsan(
        prog("      var ch := new Channel<Res>();\n"
             "      var s := ch.NewSender();\n"
             "      s.Send(new Res());\n"
             "      var h := new H();\n"
             "      h.F := s;\n"
             "      h.Dispose();\n"
             "      Console.WriteLine(ch.IsClosed);\n"
             "      Console.WriteLine(ch.Count);\n"),
        expected("True\n1\nRD\n"));
}
// Two senders through one collection: only the second release is the last.
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_both_senders_at_owning_array_elements_close_the_channel) {
    expectCleanUnderAsan(
        prog("      var ch := new Channel<int>();\n"
             "      var s := ch.NewSender();\n"
             "      var s2 := ch.NewSender();\n"
             "      var a := new OwningArray<IDisposable>();\n"
             "      a.Add(move s);\n"
             "      Console.WriteLine(ch.IsClosed);\n"
             "      a.Add(move s2);\n"
             "      a.Dispose();\n"
             "      Console.WriteLine(ch.IsClosed);\n"),
        expected("False\nTrue\n"));
}
// Not the last sender: the channel stays open and the other sender works.
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_9_one_of_two_senders_at_owned_field_leaves_the_channel_open) {
    expectCleanUnderAsan(
        prog("      var ch := new Channel<int>();\n"
             "      var s := ch.NewSender();\n"
             "      var s2 := ch.NewSender();\n"
             "      var h := new H();\n"
             "      h.F := s;\n"
             "      h.Dispose();\n"
             "      Console.WriteLine(ch.IsClosed);\n"
             "      s2.Send(3);\n"
             "      Console.WriteLine(ch.Count);\n"),
        expected("False\n1\n"));
}

// ---- the completion sources ----------------------------------------------

BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_pending_tcs_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kTcsPending), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_pending_tcs_at_owned_field) {
    expectCleanUnderAsan(atField(kTcsPending), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_pending_tcs_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kTcsPending), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_nongeneric_tcs_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kTcsVoidSet), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_nongeneric_tcs_at_owned_field) {
    expectCleanUnderAsan(atField(kTcsVoidSet), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_nongeneric_tcs_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kTcsVoidSet), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_faulted_tcs_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kTcsFaulted), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_faulted_tcs_at_owned_field) {
    expectCleanUnderAsan(atField(kTcsFaulted), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_faulted_tcs_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kTcsFaulted), expected(""));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_owning_tcs_unconsumed_result_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kOwningTcs), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_owning_tcs_unconsumed_result_at_owned_field) {
    expectCleanUnderAsan(atField(kOwningTcs), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, stdlib_6_6_owning_tcs_unconsumed_result_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kOwningTcs), expected("RD\n"));
}

// ---- an explicit `Dispose()` through the interface -----------------------
// §28.2: disposal is idempotent — the explicit call disposes, the slot's own
// scope-end discharge then finds it disposed and only reclaims the block. So
// the owned contents are disposed ONCE, at the explicit call (the marker
// after it proves the order).

namespace {
std::string explicitDispose(const std::string& setup) {
    return prog(setup +
                "      var d: IDisposable := move c;\n"
                "      d.Dispose();\n"
                "      Console.WriteLine(\"after\");\n");
}
}  // namespace
BEZEL_TEST(idisposable_slot_channel_tcs_8608, s28_2_channel_explicit_dispose_through_idisposable) {
    expectCleanUnderAsan(explicitDispose(kChannel), expected("RD\nafter\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, s28_2_sender_explicit_dispose_through_idisposable) {
    expectCleanUnderAsan(
        prog("      var ch := new Channel<int>();\n"
             "      var c := ch.NewSender();\n"
             "      var d: IDisposable := move c;\n"
             "      d.Dispose();\n"
             "      Console.WriteLine(ch.IsClosed);\n"),
        expected("True\n"));
}
BEZEL_TEST(idisposable_slot_channel_tcs_8608, s28_2_owning_tcs_explicit_dispose_through_idisposable) {
    expectCleanUnderAsan(explicitDispose(kOwningTcs), expected("RD\nafter\n"));
}

// ---- an `await` on the produced task -------------------------------------
// §30.11 / stdlib §6.6: the source's disposal "disposes the produced task,
// faulting it first if the source is disposed before completion". A task
// awaited to completion before the holder is disposed yields its result.
BEZEL_TEST(idisposable_slot_channel_tcs_8608, s30_11_tcs_awaited_then_disposed_at_owned_field) {
    expectCleanUnderAsan(
        prog("      Console.WriteLine(C.Run(i).Result);\n",
             "  public static async method Run(i: int): Task<int>\n"
             "    var tcs := new TaskCompletionSource<int>();\n"
             "    tcs.SetResult(i);\n"
             "    var v := await tcs.Task;\n"
             "    var h := new H();\n"
             "    h.F := tcs;\n"
             "    h.Dispose();\n"
             "    return v;\n"
             "  end Run;\n"),
        [] {
            std::string out;
            for (int i = 0; i < kIters; ++i) out += std::to_string(i) + "\n";
            return out + "end\n";
        }());
}
