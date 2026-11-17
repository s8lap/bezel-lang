// Spec-derived regression test for issue #6314 (narrow: the Channel group and
// the receivers that share its cause).
//
// §28.2 (use after disposal; checked disposal mode) / §28.3 (the `using var`
// scope-bound auto-drop) / §30.15 (channels), v0.357. stdlib §6.9
// (`Channel<T>` / `ChannelSender<T>`), stdlib §4.6 / §4.7 (`LinkedList<T>` /
// `OwningLinkedList<T>`), stdlib §8.1 (`Stopwatch`).
//
// THE RULE. §28.2, verbatim: "The two modes give identical observable behavior
// for every program that never uses a disposed instance." and "A further
// `Dispose()` call remains a no-op, not an error, in both modes." Every
// program below disposes a built-in explicitly and then never touches it
// again, so checked disposal mode must print exactly what unchecked mode
// prints.
//
// THE DEFECT. Fix #2967 nulls a built-in receiver's slot after its explicit
// `Dispose()` so §28.3's scope-end auto-drop reloads null and releases
// nothing. It exempted checked disposal mode wholesale, on the premise that
// checked mode "quarantines instead of freeing" and the disposed-flag guard
// must still read through the slot. That premise holds for the array-header
// and dictionary arms only (`bz_array_mark_disposed`, `bz_dict_mark_disposed`).
// A `ChannelSender<T>`, a `LinkedList<T>`, a `Random`, a `Stopwatch`, a
// `StringBuilder`, a `Task<T>` … has no disposed flag and no quarantine: its
// explicit `Dispose()` is a REAL release in checked mode too, and the kept
// pointer was released again at scope end. For `ChannelSender<T>` the handle
// was additionally released by two `Dispose` arms in a row, so even a plain
// `var snd` (no scope-end drop at all) double-freed.
//
// MEASURED on the parent commit (256ba4d8f), `bezelc --checked-disposal`: the
// four corpus cases the issue lists for this group — s30_15_0004, l6_9_0004,
// l6_9_0006, l6_9_0008 — abort with glibc "free(): double free detected in
// tcache 2" (and "AddressSanitizer: attempting double-free" under
// `--sanitize=address`); all four print the corpus's expected output in
// unchecked mode. The same holds for 100-iteration loops of
// `using var x := <T>; x.Dispose();` over `LinkedList<int>` and
// `OwningLinkedList<T>` (SEGV in the allocator), `Random`, `Stopwatch`,
// `StringBuilder` (double free) and `Task<int>` (the second task disposal
// never returns).
//
// Each row runs on the strict AOT oracle in checked disposal mode
// (`runAotCheckedDisposal`) and again in checked disposal mode under
// generated-code ASan + LSan, and asserts on STDOUT — a crashed program has
// `ran == false` and prints nothing, so no row can pass on an exit code alone.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedDisposal;

namespace {

const char* kDidNotRun = "<did-not-run>";

// §28.2 checked disposal mode, plain build.
std::string outChecked(const std::string& src) {
    auto r = runAotCheckedDisposal(src);
    return r.ran ? r.stdoutText : std::string(kDidNotRun);
}

// §28.2 checked disposal mode under generated-code AddressSanitizer +
// LeakSanitizer: the oracle that names a double free instead of depending on
// the allocator noticing one.
std::string outCheckedSan(const std::string& src) {
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = true;
    auto r = runAotChecked(src, opts);
    return r.ran ? r.stdoutText : std::string(kDidNotRun);
}

// Unchecked mode — the other half of §28.2's "identical observable behavior".
std::string outUnchecked(const std::string& src) {
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = false;
    auto r = runAot(src, opts);
    return r.ran ? r.stdoutText : std::string(kDidNotRun);
}

// The three modes must agree, and agree on `want`.
#define EXPECT_ALL_MODES(src, want)                     \
    do {                                                \
        BEZEL_EXPECT_EQ(outChecked(src), want);         \
        BEZEL_EXPECT_EQ(outCheckedSan(src), want);      \
        BEZEL_EXPECT_EQ(outUnchecked(src), want);       \
    } while (0)

// ---------------------------------------------------------------------------
// The issue's own repros: the four Channel corpus cases, verbatim.
// ---------------------------------------------------------------------------

// tests/conformance/rules/cases/s30_15_0004.bzl (§30.15).
const char* kS30_15_0004 = R"BZL(
module Conf;

import System.Threading;

public sealed class Report
  public _id: int;
  public constructor(id: int)
    _id := id;
  end constructor;
end Report;

public class Program
  public static async method Main(): Task
    using var inbox := new Channel<Report>();
    using var tx := inbox.NewSender();
    tx.Send(new Report(1));
    tx.Send(new Report(2));
    tx.Dispose();
    using var a := await inbox.Receive();
    using var b := await inbox.Receive();
    Console.WriteLine(a._id);
    Console.WriteLine(b._id);
    Console.WriteLine(inbox.IsClosed);
  end Main;
end Program;
)BZL";

// tests/conformance/rules/cases/l6_9_0004.bzl (stdlib §6.9).
const char* kL6_9_0004 = R"BZL(
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<Job>();
    var snd := ch.NewSender();
    snd.Send(new Job(1));
    snd.Send(new Job(2));
    snd.Dispose();
    using var a := await ch.Receive();
    using var b := await ch.Receive();
    Console.WriteLine(a.Id);
    Console.WriteLine(b.Id);
    try
      using var c := await ch.Receive();
    catch
      ChannelClosedException => Console.WriteLine("closed"),
    end try;
  end Main;
end Program;
)BZL";

// l6_9_0004 without its third, faulting `Receive()` — the form the LSan leg of
// that row runs. In checked disposal mode the `ChannelClosedException` object
// of a faulted `await ch.Receive()` is leaked (40 bytes, `bz_channel_receive`)
// whether or not any sender is ever disposed explicitly: MEASURED on the
// parent commit with the sender scoped by a block and no `Dispose()` call. It
// is a separate defect from this one, and the only thing LeakSanitizer says
// about the verbatim case.
const char* kL6_9_0004NoFault = R"BZL(
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<Job>();
    var snd := ch.NewSender();
    snd.Send(new Job(1));
    snd.Send(new Job(2));
    snd.Dispose();
    using var a := await ch.Receive();
    using var b := await ch.Receive();
    Console.WriteLine(a.Id);
    Console.WriteLine(b.Id);
  end Main;
end Program;
)BZL";

// tests/conformance/rules/cases/l6_9_0006.bzl (stdlib §6.9).
const char* kL6_9_0006 = R"BZL(
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public class Program
  public static method Main()
    using var ch := new Channel<Job>();
    using var snd := ch.NewSender();
    Console.WriteLine(ch.TryReceive(out _));
    Console.WriteLine(ch.IsClosed);
    snd.Dispose();
    Console.WriteLine(ch.TryReceive(out _));
    Console.WriteLine(ch.IsClosed);
  end Main;
end Program;
)BZL";

// tests/conformance/rules/cases/l6_9_0008.bzl (stdlib §6.9).
const char* kL6_9_0008 = R"BZL(
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<Job>();
    var snd := ch.NewSender();
    snd.Send(new Job(1));
    snd.Send(new Job(2));
    snd.Send(new Job(3));
    snd.Dispose();
    var total := 0;
    await for var j in ch.ReceiveAll() do
      using var jj := move j;
      total := total + jj.Id;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
)BZL";

// ---------------------------------------------------------------------------
// The same shapes looped 100x, so one lucky allocator state cannot pass.
// ---------------------------------------------------------------------------

// l6_9_0006's shape: `using var snd` + explicit `Dispose()`. Disposing the last
// sender closes the channel (§30.15), observed 100 times.
const char* kSenderUsingVarLoop = R"BZL(
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public class Program
  public static method Main()
    var shut := 0;
    var i := 0;
    while i < 100 do
      using var ch := new Channel<Job>();
      using var snd := ch.NewSender();
      snd.Dispose();
      if ch.IsClosed then
        shut := shut + 1;
      end;
      i := i + 1;
    end;
    Console.WriteLine(shut);
  end Main;
end Program;
)BZL";

// l6_9_0004's shape: a plain `var snd` (no scope-end drop), items sent before
// the explicit `Dispose()` stay receivable (§30.15 "the channel drains").
const char* kSenderVarLoop = R"BZL(
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public class Program
  public static async method Main(): Task
    var total := 0;
    var shut := 0;
    var i := 0;
    while i < 100 do
      using var ch := new Channel<Job>();
      var snd := ch.NewSender();
      snd.Send(new Job(i));
      snd.Dispose();
      using var a := await ch.Receive();
      total := total + a.Id;
      if ch.IsClosed then
        shut := shut + 1;
      end;
      i := i + 1;
    end;
    Console.WriteLine(total);
    Console.WriteLine(shut);
  end Main;
end Program;
)BZL";

// A sync `Main` that runs `body` 100 times and prints how many iterations
// completed.
std::string loop100(const std::string& body, const std::string& prelude = "") {
    return "module M; import System.Diagnostics; import System.Threading; " +
           prelude +
           "class C public static method Main(): int "
           "  var n := 0; "
           "  var i := 0; "
           "  while i < 100 do " +
           body +
           "    n := n + 1; "
           "    i := i + 1; "
           "  end; "
           "  Console.WriteLine(n); "
           "  return 0; "
           "end Main; end C;";
}

// The §28.2 / §28.3 pair: an explicit `Dispose()` on a `using var`.
std::string bothSpellings(const std::string& ctor, const std::string& prelude = "") {
    return loop100("using var x := " + ctor + "; x.Dispose(); ", prelude);
}

// §28.2: "A further `Dispose()` call remains a no-op, not an error, in both
// modes."
std::string disposedTwice(const std::string& ctor) {
    return loop100("using var x := " + ctor + "; x.Dispose(); x.Dispose(); ");
}

// A class element with a live-instance counter, for the owning list row.
const char* kSrc =
    "class Src "
    "  public static _live: int := 0; "
    "  public static _dtor: int := 0; "
    "  private _v: int := 5; "
    "  public constructor() Src._live := Src._live + 1; end constructor; "
    "  destructor "
    "    Src._live := Src._live - 1; "
    "    Src._dtor := Src._dtor + 1; "
    "  end destructor; "
    "end Src; ";

}  // namespace

// ---------------------------------------------------------------------------
// The four corpus cases issue #6314 lists for the Channel group.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_checked_dispose_release_once_6314, corpus_s30_15_0004_sender_dispose_then_drain_30_15) {
    EXPECT_ALL_MODES(kS30_15_0004, "1\n2\nTrue\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, corpus_l6_9_0004_plain_var_sender_dispose_then_closed_stdlib_6_9) {
    // The verbatim case, in both modes (the double free aborted it before any
    // output on the parent commit).
    BEZEL_EXPECT_EQ(outChecked(kL6_9_0004), "1\n2\nclosed\n");
    BEZEL_EXPECT_EQ(outUnchecked(kL6_9_0004), "1\n2\nclosed\n");
    // The memory oracle, on the same sender shape (see kL6_9_0004NoFault).
    EXPECT_ALL_MODES(kL6_9_0004NoFault, "1\n2\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, corpus_l6_9_0006_sender_dispose_closes_channel_stdlib_6_9) {
    EXPECT_ALL_MODES(kL6_9_0006, "False\nFalse\nFalse\nTrue\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, corpus_l6_9_0008_receive_all_after_sender_dispose_stdlib_6_9) {
    EXPECT_ALL_MODES(kL6_9_0008, "6\n");
}

// ---------------------------------------------------------------------------
// The Channel group, looped 100x.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_checked_dispose_release_once_6314, sender_using_var_explicit_dispose_100x_28_2) {
    EXPECT_ALL_MODES(kSenderUsingVarLoop, "100\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, sender_plain_var_explicit_dispose_100x_28_2) {
    EXPECT_ALL_MODES(kSenderVarLoop, "4950\n100\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, sender_disposed_twice_is_a_no_op_100x_28_2) {
    EXPECT_ALL_MODES(
        loop100("using var ch := new Channel<int>(); "
                "using var x := ch.NewSender(); "
                "x.Dispose(); "
                "x.Dispose(); "),
        "100\n");
}

// ---------------------------------------------------------------------------
// The receivers that share the cause: an explicit `Dispose()` whose release
// does not quarantine, on a `using var`.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_checked_dispose_release_once_6314, linked_list_explicit_then_autodrop_100x_28_2) {
    EXPECT_ALL_MODES(bothSpellings("new LinkedList<int>()"), "100\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, owning_linked_list_disposes_elements_exactly_once_100x_28_2) {
    const std::string src =
        "module M; " + std::string(kSrc) +
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 100 do "
        "    using var x := new OwningLinkedList<Src>(); "
        "    x.AddLast(new Src()); "
        "    x.Dispose(); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine($\"live={Src._live} dtor={Src._dtor}\"); "
        "  return 0; "
        "end Main; end C;";
    EXPECT_ALL_MODES(src, "live=0 dtor=100\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, random_explicit_then_autodrop_100x_28_2) {
    EXPECT_ALL_MODES(bothSpellings("new Random(1)"), "100\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, stopwatch_explicit_then_autodrop_100x_28_2) {
    EXPECT_ALL_MODES(bothSpellings("new Stopwatch()"), "100\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, string_builder_explicit_then_autodrop_100x_28_2) {
    EXPECT_ALL_MODES(bothSpellings("new StringBuilder()"), "100\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, event_source_explicit_then_autodrop_100x_28_2) {
    EXPECT_ALL_MODES(bothSpellings("new EventSource<int>()"), "100\n");
}

// A collection behind `ICollection<T>` is released through `bz_runtime_dispose`
// — a real release in checked mode, unlike the `IList<T>` arm's quarantine.
BEZEL_TEST(codegen_checked_dispose_release_once_6314, collection_interface_explicit_then_autodrop_100x_28_2) {
    EXPECT_ALL_MODES(
        loop100("using var x: ICollection<int> := new Array<int>(2); x.Dispose(); "),
        "100\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, task_explicit_then_autodrop_100x_28_2) {
    EXPECT_ALL_MODES(bothSpellings("Task.FromResult(2)"), "100\n");
}

// §28.2: a further `Dispose()` is a no-op in both modes.
BEZEL_TEST(codegen_checked_dispose_release_once_6314, random_disposed_twice_is_a_no_op_100x_28_2) {
    EXPECT_ALL_MODES(disposedTwice("new Random(1)"), "100\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, stopwatch_disposed_twice_is_a_no_op_100x_28_2) {
    EXPECT_ALL_MODES(disposedTwice("new Stopwatch()"), "100\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, linked_list_disposed_twice_is_a_no_op_100x_28_2) {
    EXPECT_ALL_MODES(disposedTwice("new LinkedList<int>()"), "100\n");
}

// ---------------------------------------------------------------------------
// THE TWO BOUNDARY CONTROLS — the receivers whose checked-mode release DOES
// quarantine (the array-header arm and the dictionary arm) keep their
// pointer, so §28.2's checked-mode backstop still reads the disposed flag
// through the slot: "any instance member access on a logically-disposed
// instance … raises `DisposedAccessException`". These hold on the parent
// commit too. WRONG FIX they catch: nulling the slot of EVERY explicitly
// disposed receiver in checked mode (dropping the exemption wholesale) — the
// guard then reads through a null slot and the access no longer raises.
// Plain checked mode only: a quarantined header that a
// `DisposedAccessException` unwinds past is reported by LeakSanitizer on about
// half the runs, on the parent commit as well, so an LSan leg here would be a
// coin toss about a different defect.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_checked_dispose_release_once_6314, control_array_keeps_checked_backstop_28_2) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    using var x := new Array<int>(2); "
        "    x.Dispose(); "
        "    Console.WriteLine(x.Count); "
        "    Console.WriteLine(\"no-raise\"); "
        "  catch "
        "    DisposedAccessException => Console.WriteLine(\"DAE\"), "
        "  end try; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(outChecked(src), "DAE\n");
}

BEZEL_TEST(codegen_checked_dispose_release_once_6314, control_dictionary_keeps_checked_backstop_28_2) {
    const std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    using var x := new Dictionary<string, int>(); "
        "    x.Dispose(); "
        "    x.Add(\"a\", 1); "
        "    Console.WriteLine(\"no-raise\"); "
        "  catch "
        "    DisposedAccessException => Console.WriteLine(\"DAE\"), "
        "  end try; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(outChecked(src), "DAE\n");
}
