// Codegen + runtime — issue #3261: `Exception.SupersededChain` is unimplemented
// (spec §31.2 / §31.4, v0.357).
//
// THE SPEC.
//
// §31.4 (v0.357), the base `Exception` surface, verbatim:
//
//     public readonly property SupersededChain: Array<ExceptionInfo>;   // empty
//                                                                            // when
//                                                                            // nothing
//                                                                            // superseded
//
// and: "`SupersededChain` carries an `ExceptionInfo` snapshot of every exception
// this exception **superseded** — replaced while it was propagating (§31.2's
// record-before-drop rule states when and how the runtime appends). … No
// constructor populates or accepts it — the chain is runtime-populated exactly
// as `StackTrace` is, so no constructor's §22.15 purity classification changes;
// it starts empty and shares the empty-array representation, so an exception
// that supersedes nothing allocates nothing."
//
// §31.2 (v0.357), the record-before-drop rule, verbatim: "At the instant of
// supersession … and before the superseded exception is disposed … the runtime
// appends to the **superseding** exception's `SupersededChain`: first, in
// order, every entry of the superseded exception's own `SupersededChain`, and
// then, last, an `ExceptionInfo` snapshot of the superseded exception itself —
// its type name, `Message`, and `StackTrace`. Entries appear in **supersession
// order, earliest first**: the first exception lost is the first entry."
//
// WHAT WAS WRONG. The member did not exist on the `Exception` surface at all:
// `grep -rn "SupersededChain" src/` found ONE hit, a comment. Every program in
// the rows below was REJECTED at compile time with
// "no member `SupersededChain` on `Exception`; documented surface: Message,
// StackTrace, CauseChain and the universal `ToString`/`Equals`/`GetHashCode`/
// `GetType` (§8.1)" — the member, the object slot it reads, and the runtime
// append that fills it were all absent.
//
// THE FIX. (1) The §31.4 header grows a `SupersededChain` slot at byte 40
// (the block grows 40 → 48 bytes; the user-subclass mirror in `classStruct`
// grows with it, and its disposed flag moves from slot 5 to slot 6). (2) The
// member resolves in sema and lowers to that slot. (3) The runtime helper
// `bz_exc_superseded_append` implements §31.2's append — the superseded
// exception's own chain first, then its own snapshot — and is called at each
// supersession point BEFORE the superseded exception is disposed.
//
// The rows below are the issue's own repro plus the matrix around it: no
// supersession, a disposer throwing during the unwind, a `finally` throwing,
// a nested two-deep supersession, and an async body. The oracle is
// `runAotChecked` (the generated code is ASan-instrumented), so a leak or a
// use-after-free introduced by the new slot fails the row.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked / runAot — AOT oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ISSUE #3261's OWN REPRO (the reduction in the issue's second comment),
// reduced to the observable §31.2 pins: a `finally` that throws supersedes
// the in-flight exception, and the superseding exception's
// `SupersededChain[0].Message` is the superseded one's ("first").
//
// Before the fix this program was REJECTED:
//   "no member `SupersededChain` on `InvalidOperationException`; documented
//    surface: Message, StackTrace, CauseChain …"
//
// `ExA` / `ExB` are USER SUBCLASSES of `Exception`, so each carries the §31.4
// header mirror (Message@8 / InnerException@16 / CauseChain@24 / ParamName@32
// / SupersededChain@40) — the same layout the built-in block uses.
//
// The catch arm's body is a `do … end` block: an arm body with more than one
// statement is written that way (a single-statement arm is the bare
// `=> Console.WriteLine(…)` form).
const char* kFinallyThrows =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class Program "
    "  public static method Main() "
    "    try "
    "      try "
    "        throw new ExA(\"first\"); "
    "      finally "
    "        throw new ExB(\"second\"); "
    "      end try; "
    "    catch "
    "      var ex: Exception => do "
    "        Console.WriteLine(ex.SupersededChain.Count); "
    "        Console.WriteLine(ex.SupersededChain[0].Message); "
    "        Console.WriteLine(ex.SupersededChain[0].TypeName); "
    "      end end; "
    "  end Main; "
    "end Program; ";

// §31.4: "it starts empty and shares the empty-array representation, so an
// exception that supersedes nothing allocates nothing." The chain of an
// exception that superseded nothing is EMPTY.
const char* kNoSupersession =
    "module M; "
    "public class Program "
    "  public static method Main() "
    "    try "
    "      throw new ArgumentException(\"boom\"); "
    "    catch "
    "      var ex: Exception => Console.WriteLine(ex.SupersededChain.Count); "
    "    end try; "
    "  end Main; "
    "end Program; ";

// §31.2's ORDER rule: "Entries appear in supersession order, earliest first:
// the first exception lost is the first entry." Two-deep: ExB supersedes ExA
// (ExB's chain is [snap(ExA)]), then ExC supersedes ExB (ExC's chain is
// ExB's own chain FIRST, then snap(ExB)) — so ExC reads [first, second].
const char* kNestedTwoDeep =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class ExC: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExC; "
    "public class Program "
    "  public static method Main() "
    "    try "
    "      try "
    "        try "
    "          throw new ExA(\"first\"); "
    "        finally "
    "          throw new ExB(\"second\"); "
    "        end try; "
    "      finally "
    "        throw new ExC(\"third\"); "
    "      end try; "
    "    catch "
    "      var ex: Exception => do "
    "        Console.WriteLine(ex.SupersededChain.Count); "
    "        Console.WriteLine(ex.SupersededChain[0].Message); "
    "        Console.WriteLine(ex.SupersededChain[1].Message); "
    "      end end; "
    "  end Main; "
    "end Program; ";

// §28.4's multi-value cleanup: a DISPOSER that throws while an exception is
// propagating supersedes the in-flight exception, and the superseded one is
// recorded into the superseding one's chain before the drop.
const char* kDisposerThrowsDuringUnwind =
    "module M; "
    "public class Boom "
    "  public constructor() end constructor; "
    "  destructor "
    "    throw new InvalidOperationException(\"cleanup\"); "
    "  end destructor; "
    "end Boom; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class Program "
    "  public static method Main() "
    "    try "
    "      using var b := new Boom(); "
    "      throw new ExA(\"first\"); "
    "    catch "
    "      var ex: Exception => do "
    "        Console.WriteLine(ex.Message); "
    "        Console.WriteLine(ex.SupersededChain.Count); "
    "        Console.WriteLine(ex.SupersededChain[0].Message); "
    "      end end; "
    "  end Main; "
    "end Program; ";

// §30.9's ASYNC body: the same supersession inside an `async` method. The
// first row's `finally` has no await (the synchronous finally path); the
// second's awaits (the `finallyAwaits` path, which spills the in-flight
// exception into a frame-backed slot across the suspension).
const char* kAsyncFinallyThrows =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class Program "
    "  public static async method Main(): Task<int> "
    "    try "
    "      try "
    "        throw new ExA(\"first\"); "
    "      finally "
    "        throw new ExB(\"second\"); "
    "      end try; "
    "    catch "
    "      var ex: Exception => do "
    "        Console.WriteLine(ex.SupersededChain.Count); "
    "        Console.WriteLine(ex.SupersededChain[0].Message); "
    "      end end; "
    "    return 0; "
    "  end Main; "
    "end Program; ";

const char* kAsyncFinallyAwaits =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class Program "
    "  public static async method Main(): Task<int> "
    "    try "
    "      try "
    "        throw new ExA(\"first\"); "
    "      finally "
    "        await Task.Delay(1); "
    "        throw new ExB(\"second\"); "
    "      end try; "
    "    catch "
    "      var ex: Exception => do "
    "        Console.WriteLine(ex.SupersededChain.Count); "
    "        Console.WriteLine(ex.SupersededChain[0].Message); "
    "      end end; "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// §31.4: "No constructor populates or accepts `SupersededChain`". The
// three-argument `new Exception(...)` has no matching constructor, so the
// program is REJECTED — and the rejection names the constructor surface. This
// row is the corpus case `s31_4_0007` (banked `pass`): it must STAY pass.
const char* kNoConstructorAcceptsChain =
    "module M; "
    "public class Program "
    "  public static method Main() "
    "    using var cause := new Exception(\"cause\"); "
    "    using var e := new Exception(\"msg\", cause, cause.SupersededChain); "
    "  end Main; "
    "end Program; ";

// The stdout a program printed, with the trailing newline stripped.
std::string out(const JitResult& r) {
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// The issue's own repro: a `finally` that throws supersedes the in-flight
// exception, and the superseded one's snapshot is in the chain.
// =====================================================================

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           finally_throwing_records_superseded_spec_31_2_31_4) {
    JitResult r = runAotChecked(kFinallyThrows);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // count 1, the superseded message "first", the superseded type name.
    BEZEL_EXPECT_EQ(out(r), std::string("1\nfirst\nM.ExA"));
}

// =====================================================================
// §31.4: the chain is EMPTY on an exception that superseded nothing.
// =====================================================================

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           empty_chain_when_nothing_superseded_spec_31_4) {
    JitResult r = runAotChecked(kNoSupersession);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("0"));
}

// =====================================================================
// §31.2: the entries read EARLIEST FIRST, at every depth.
// =====================================================================

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           nested_two_deep_earliest_first_spec_31_2) {
    JitResult r = runAotChecked(kNestedTwoDeep);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("2\nfirst\nsecond"));
}

// =====================================================================
// §28.4: a disposer that throws during the unwind supersedes the in-flight
// exception, and the record happens BEFORE the drop.
// =====================================================================

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           disposer_throwing_during_unwind_spec_31_2_28_4) {
    JitResult r = runAotChecked(kDisposerThrowsDuringUnwind);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("cleanup\n1\nfirst"));
}

// =====================================================================
// §30.9: the same supersession inside an async body (the synchronous
// `finally` path and the `finallyAwaits` path).
// =====================================================================

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           async_body_finally_throwing_spec_31_2_30_9) {
    JitResult r = runAotChecked(kAsyncFinallyThrows);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("1\nfirst"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           async_body_finally_awaiting_spec_31_2_30_9) {
    JitResult r = runAotChecked(kAsyncFinallyAwaits);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("1\nfirst"));
}

// =====================================================================
// §31.4: no constructor accepts the chain — the reject still stands (the
// corpus case `s31_4_0007`, banked `pass`).
// =====================================================================

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           no_constructor_accepts_superseded_chain_spec_31_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(kNoConstructorAcceptsChain, "constructor"));
}

// =====================================================================
// fix-forward of #3261 (review r42-rev-m13-3261, findings F2 and F4).
// =====================================================================

namespace {

// F2. §31.2: the record is "a copy of value data" — the snapshot of a user
// exception class that OVERRIDES `ToString`. The snapshot's `TypeName` is read
// through the virtual `ToString` (vtable slot 3, the CauseChain channel; the
// value is #8634's defect and is deliberately NOT pinned here). The override
// returns a FRESH string, and the append retained that owned (+1) result a
// second time: LeakSanitizer reported 9000 bytes in 400 allocations over the
// 200 iterations. The row pins the count and the superseded `Message`, and
// `runAotChecked` fails it on the leak.
const char* kToStringOverrideLoop =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "  public override method ToString(): string "
    "    return \"CUSTOM-\" + Message; "
    "  end ToString; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class Program "
    "  public static method Main() "
    "    for var i in 0..<200 do "
    "      try "
    "        try "
    "          throw new ExA(\"first\"); "
    "        finally "
    "          throw new ExB(\"second\"); "
    "        end try; "
    "      catch "
    "        var ex: Exception => do "
    "          if i = 199 then "
    "            Console.WriteLine(ex.SupersededChain.Count); "
    "            Console.WriteLine(ex.SupersededChain[0].Message); "
    "          end if; "
    "        end end; "
    "    end for; "
    "  end Main; "
    "end Program; ";

// F4. §31.2: "when a `finally` entered during exception propagation itself
// completes by throwing (directly, or via a call that propagates), the new
// exception replaces the one being handled". Awaiting a faulted task re-raises
// its fault (§30.9), so an `await` of a faulted task inside such a `finally`
// — or inside a catch arm — supersedes the in-flight / caught `ExB`: the
// re-raised fault's `SupersededChain` records `ExB` and `ExB` is disposed.
// At the reviewed tip 0823dd306 the chain was empty (count 0) and `ExB`
// leaked (48 bytes). On this base the await's unwind edge reaches the region's
// leave block (#7940), whose emitEndExcHandling now records the supersession.
const char* kFinallyAwaitsFaultedTask =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class Program "
    "  public static async method Fail(): Task<int> "
    "    throw new ExA(\"task-fault\"); "
    "  end Fail; "
    "  public static async method Main(): Task<int> "
    "    var t := Program.Fail(); "
    "    try "
    "      try "
    "        throw new ExB(\"loss\"); "
    "      finally "
    "        await t; "
    "      end try; "
    "    catch "
    "      var e: Exception => do "
    "        Console.WriteLine(e.Message); "
    "        Console.WriteLine(e.SupersededChain.Count); "
    "        Console.WriteLine(e.SupersededChain[0].Message); "
    "      end end; "
    "    return 0; "
    "  end Main; "
    "end Program; ";

const char* kCatchArmAwaitsFaultedTask =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class Program "
    "  public static async method Fail(): Task<int> "
    "    throw new ExA(\"task-fault\"); "
    "  end Fail; "
    "  public static async method Main(): Task<int> "
    "    var t := Program.Fail(); "
    "    try "
    "      try "
    "        throw new ExB(\"loss\"); "
    "      catch "
    "        var b: ExB => do await t; end end; "
    "    catch "
    "      var e: Exception => do "
    "        Console.WriteLine(e.Message); "
    "        Console.WriteLine(e.SupersededChain.Count); "
    "        Console.WriteLine(e.SupersededChain[0].Message); "
    "      end end; "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// F4. §31.2: "The chain is per raise, not per instance: when an owner
// re-raises one of those instances into the in-flight slot, its
// `SupersededChain` is cleared first, so it records only the supersessions of
// the raise being reported." The SAME task-owned fault is re-raised three
// times (§30.4: a `Task` may be awaited again), each raise superseding one
// `ExB`; each raise reports exactly one entry. Without the clear the chain
// accumulates across raises (1, 2, 3).
const char* kMultiAwaitPerRaise =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class Program "
    "  public static async method Fail(): Task<int> "
    "    throw new ExA(\"task-fault\"); "
    "  end Fail; "
    "  public static async method Main(): Task<int> "
    "    var t := Program.Fail(); "
    "    for var i in 0..<3 do "
    "      try "
    "        try "
    "          throw new ExB(\"loss\" + i.ToString()); "
    "        finally "
    "          await t; "
    "        end try; "
    "      catch "
    "        var e: ExA => do "
    "          Console.WriteLine(e.SupersededChain.Count); "
    "          Console.WriteLine(e.SupersededChain[0].Message); "
    "        end end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// The same clause on §30.3's other re-raise, the faulted `Result` read.
const char* kResultReadPerRaise =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class Program "
    "  public static async method Fail(): Task<int> "
    "    throw new ExA(\"task-fault\"); "
    "  end Fail; "
    "  public static method Main() "
    "    var t := Program.Fail(); "
    "    for var i in 0..<3 do "
    "      try "
    "        try "
    "          throw new ExB(\"loss\" + i.ToString()); "
    "        finally "
    "          var r := t.Result; "
    "        end try; "
    "      catch "
    "        var e: ExA => do "
    "          Console.WriteLine(e.SupersededChain.Count); "
    "          Console.WriteLine(e.SupersededChain[0].Message); "
    "        end end; "
    "    end for; "
    "  end Main; "
    "end Program; ";

// The same clause on the other owner-owned instance, the cached
// `TypeInitializationException` (§22.7: every later access re-raises the SAME
// cached instance).
const char* kCachedTiePerRaise =
    "module M; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class Cfg "
    "  public static readonly Value: int := Cfg.Load(); "
    "  private static method Load(): int "
    "    throw new InvalidOperationException(\"init failed\"); "
    "  end Load; "
    "end Cfg; "
    "public class Program "
    "  public static method Main() "
    "    for var i in 0..<3 do "
    "      try "
    "        try "
    "          throw new ExB(\"loss\" + i.ToString()); "
    "        finally "
    "          Console.WriteLine(Cfg.Value); "
    "        end try; "
    "      catch "
    "        var e: TypeInitializationException => do "
    "          Console.WriteLine(e.SupersededChain.Count); "
    "          Console.WriteLine(e.SupersededChain[0].Message); "
    "        end end; "
    "    end for; "
    "  end Main; "
    "end Program; ";

}  // namespace

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           tostring_override_snapshot_is_leak_free_spec_31_2) {
    JitResult r = runAotChecked(kToStringOverrideLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("1\nfirst"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           finally_await_faulted_task_supersedes_spec_31_2_30_9) {
    JitResult r = runAotChecked(kFinallyAwaitsFaultedTask);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("task-fault\n1\nloss"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           catch_arm_await_faulted_task_supersedes_spec_31_2_30_9) {
    JitResult r = runAotChecked(kCatchArmAwaitsFaultedTask);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("task-fault\n1\nloss"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           multi_await_chain_is_per_raise_spec_31_2_30_4) {
    JitResult r = runAotChecked(kMultiAwaitPerRaise);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("1\nloss0\n1\nloss1\n1\nloss2"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           result_read_chain_is_per_raise_spec_31_2_30_3) {
    JitResult r = runAotChecked(kResultReadPerRaise);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("1\nloss0\n1\nloss1\n1\nloss2"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           cached_tie_chain_is_per_raise_spec_31_2_22_7) {
    JitResult r = runAotChecked(kCachedTiePerRaise);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("1\nloss0\n1\nloss1\n1\nloss2"));
}

// =====================================================================
// fix-forward of #3261 (review r42-rev-m13d-3261, finding G1): A BORROW OF
// THE CHAIN OUTLIVES THE RE-RAISE AND THE APPEND THAT REPLACE IT.
// =====================================================================
//
// §31.4 (v0.357) declares `public readonly property SupersededChain:
// Array<ExceptionInfo>`, so `e.SupersededChain` is a borrow of the chain
// array, and §31.1 (v0.357) bounds it by the arm's binding: "the bound
// exception variable … is a borrow of the in-flight exception; its source is
// the runtime, and its lifetime is the arm body's". §31.2 (v0.357) changes the
// chain while that borrow is live, twice over: "the runtime appends to the
// **superseding** exception's `SupersededChain`" at every supersession, and
// "when an owner re-raises one of those instances into the in-flight slot, its
// `SupersededChain` is cleared first". Neither sentence ends the borrow.
//
// WHAT WAS WRONG. The reader returned the raw slot pointer, the clear freed
// the array and released its strings in place, and the append installed a new
// array and freed the old one. A chain borrowed before a later owner re-raise
// (or a later supersession by the same instance) dangled: AddressSanitizer
// reported heap-use-after-free in the first four programs below, freed by
// bz_exc_superseded_clear (three of them) and bz_exc_superseded_append (the
// two-wrapper one). The base rejects every one of them (the member does not
// exist there), so each was a new unsound admission.
//
// THE FIX. An array that was lent out is retired, not freed, and reclaimed
// with the exception at its terminal reclaim (never, for the cached
// TypeInitializationException, which is not reclaimed). The borrow keeps
// reading the chain of the raise it was read in; a fresh `e.SupersededChain`
// read returns the current chain. Each program loops 200 times with a fresh
// task (or the one cached TypeInitializationException), so `runAotChecked`
// also fails the row if a retired array is never reclaimed with its task's
// fault.

namespace {

// A LOCAL borrow across a task's later re-raise. `Fail`'s body raises ExB
// over ExA("first"), so the fault's chain is [first]; the arm borrows it, then
// awaits the task again from a `finally` that ExX("x") entered — an owner
// re-raise (§30.9), which clears the chain and then records ExX. The borrow
// still reads [first]; a fresh read reads [x].
const char* kLocalBorrowAcrossReraise =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class ExX: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExX; "
    "public class Program "
    "  public static async method Fail(): Task<int> "
    "    try "
    "      throw new ExA(\"first\"); "
    "    finally "
    "      throw new ExB(\"body-raise\"); "
    "    end try; "
    "  end Fail; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var i in 0..<200 do "
    "      var t := Program.Fail(); "
    "      try "
    "        await t; "
    "      catch "
    "        var e: ExB => do "
    "          var ch := e.SupersededChain; "
    "          try "
    "            try "
    "              throw new ExX(\"x\"); "
    "            finally "
    "              await t; "
    "            end try; "
    "          catch "
    "            var e2: ExB => do "
    "              total := total + e2.SupersededChain.Count; "
    "            end "
    "          end try; "
    "          total := total + ch.Count + ch[0].Message.Length; "
    "          if i = 199 then "
    "            Console.WriteLine(ch[0].Message); "
    "            Console.WriteLine(e.SupersededChain[0].Message); "
    "          end if; "
    "        end end; "
    "    end for; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// A `for` over the chain across a task's later re-raise. The fault's chain is
// [first, second] (a two-deep supersession in the body). The loop body awaits
// the task again on every iteration: a re-raise that clears the chain the loop
// is walking. The loop still visits both entries.
const char* kForOverChainAcrossReraise =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class ExX: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExX; "
    "public class Program "
    "  public static async method Fail(): Task<int> "
    "    try "
    "      try "
    "        throw new ExA(\"first\"); "
    "      finally "
    "        throw new ExX(\"second\"); "
    "      end try; "
    "    finally "
    "      throw new ExB(\"body-raise\"); "
    "    end try; "
    "  end Fail; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var i in 0..<200 do "
    "      var t := Program.Fail(); "
    "      try "
    "        await t; "
    "      catch "
    "        var e: ExB => do "
    "          for var info in e.SupersededChain do "
    "            try "
    "              await t; "
    "            catch "
    "              var e2: ExB => do "
    "                total := total + e2.SupersededChain.Count; "
    "              end "
    "            end try; "
    "            total := total + info.Message.Length; "
    "            if i = 199 then "
    "              Console.WriteLine(info.Message); "
    "            end if; "
    "          end for; "
    "        end end; "
    "    end for; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// A local borrow across an APPEND. `Wrap` awaits `t` and lets its fault
// escape, so each wrapper task carries the SAME task-owned instance (§30.9:
// "A fault that escapes into a task is retained through its operand task").
// Awaiting `w1` from a `finally` that ExX("x1") entered records x1; the arm
// borrows that chain; awaiting `w2` from a `finally` that ExX("x2") entered
// makes the same instance supersede again, so the runtime appends to the
// chain the arm is holding. The borrow still reads [x1]. (The row reads only
// entry 0 of the fresh chain, which is "x1" or "x2" — two characters either
// way — so it does not pin #8640's open question of whether `w2`'s re-raise
// clears first.)
const char* kLocalBorrowAcrossAppend =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class ExX: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExX; "
    "public class Program "
    "  public static async method Fail(): Task<int> "
    "    throw new ExB(\"fault\"); "
    "  end Fail; "
    "  public static async method Wrap(t: Task<int>): Task<int> "
    "    return await t; "
    "  end Wrap; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var i in 0..<200 do "
    "      var t := Program.Fail(); "
    "      var w1 := Program.Wrap(t); "
    "      var w2 := Program.Wrap(t); "
    "      try "
    "        try "
    "          throw new ExX(\"x1\"); "
    "        finally "
    "          await w1; "
    "        end try; "
    "      catch "
    "        var e: ExB => do "
    "          var ch := e.SupersededChain; "
    "          try "
    "            try "
    "              throw new ExX(\"x2\"); "
    "            finally "
    "              await w2; "
    "            end try; "
    "          catch "
    "            var e2: ExB => do "
    "              total := total + e2.SupersededChain[0].Message.Length; "
    "            end "
    "          end try; "
    "          total := total + ch.Count + ch[0].Message.Length; "
    "          if i = 199 then "
    "            Console.WriteLine(ch[0].Message); "
    "          end if; "
    "        end end; "
    "    end for; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// The cached TypeInitializationException (§22.7: "the triggering access and
// every subsequent access see the same cached `TypeInitializationException`
// instance"). Each iteration's first touch supersedes ExB("loss") and the arm
// borrows that chain; the second touch, inside the arm, is an owner re-raise
// that clears it — and the inner arm's exit ends a handling of the cached
// instance while the outer arm still holds the borrow. The borrow still reads
// [loss]. The instance is never reclaimed, so its retired arrays stay
// reachable from it until exit.
const char* kCachedTieBorrowAcrossReraise =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class ExX: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExX; "
    "public class Cfg "
    "  public static readonly Value: int := Cfg.Load(); "
    "  private static method Load(): int "
    "    throw new InvalidOperationException(\"init failed\"); "
    "  end Load; "
    "end Cfg; "
    "public class Program "
    "  public static method Main() "
    "    var total := 0; "
    "    for var i in 0..<200 do "
    "      try "
    "        try "
    "          throw new ExB(\"loss\"); "
    "        finally "
    "          Console.WriteLine(Cfg.Value); "
    "        end try; "
    "      catch "
    "        var e: TypeInitializationException => do "
    "          var ch := e.SupersededChain; "
    "          try "
    "            Console.WriteLine(Cfg.Value); "
    "          catch "
    "            var e2: TypeInitializationException => do "
    "              total := total + e2.SupersededChain.Count; "
    "            end "
    "          end try; "
    "          total := total + ch.Count + ch[0].Message.Length; "
    "          if i = 199 then "
    "            Console.WriteLine(ch[0].Message); "
    "          end if; "
    "        end end; "
    "    end for; "
    "    Console.WriteLine(total); "
    "  end Main; "
    "end Program; ";

// THE OTHER TWO READER SPELLINGS. The chain is read at three sites in the
// backend: `e.SupersededChain` (the four programs above), the null-conditional
// `q?.SupersededChain`, and the bare identifier inside a method of the
// exception's own class. Each returned the raw slot pointer, so each dangled
// the same way; a fix that covers only the `.` spelling leaves these two red.

// `q?.SupersededChain` over a nullable borrow of the fault, across the task's
// re-raise (an inner `await t` with nothing in flight: the fresh chain is
// empty).
const char* kNullCondBorrowAcrossReraise =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExB; "
    "public class Program "
    "  public static async method Fail(): Task<int> "
    "    try "
    "      throw new ExA(\"first\"); "
    "    finally "
    "      throw new ExB(\"body-raise\"); "
    "    end try; "
    "  end Fail; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var i in 0..<200 do "
    "      var t := Program.Fail(); "
    "      try "
    "        await t; "
    "      catch "
    "        var e: ExB => do "
    "          var q: ExB? := e; "
    "          var ch := q?.SupersededChain; "
    "          try "
    "            await t; "
    "          catch "
    "            var e2: ExB => do "
    "              total := total + e2.SupersededChain.Count; "
    "            end "
    "          end try; "
    "          if ch <> null then "
    "            total := total + ch.Count + ch[0].Message.Length; "
    "            if i = 199 then "
    "              Console.WriteLine(ch[0].Message); "
    "            end if; "
    "          end if; "
    "        end end; "
    "    end for; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// The bare identifier `SupersededChain` inside a method of the exception's
// own class, across a `.Result` read of the faulted task (§30.3: the same
// owner re-raise as an `await`).
const char* kBareIdentifierBorrowAcrossReraise =
    "module M; "
    "public class ExA: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "end ExA; "
    "public class ExB: Exception "
    "  public constructor(message: string) base(message); "
    "  end constructor; "
    "  public method LossAcross(t: Task<int>): int "
    "    var ch := SupersededChain; "
    "    var seen := 0; "
    "    try "
    "      var r := t.Result; "
    "      seen := r; "
    "    catch "
    "      var e2: ExB => do "
    "        seen := e2.SupersededChain.Count; "
    "      end "
    "    end try; "
    "    return seen + ch.Count + ch[0].Message.Length; "
    "  end LossAcross; "
    "end ExB; "
    "public class Program "
    "  public static async method Fail(): Task<int> "
    "    try "
    "      throw new ExA(\"first\"); "
    "    finally "
    "      throw new ExB(\"body-raise\"); "
    "    end try; "
    "  end Fail; "
    "  public static async method Main(): Task<int> "
    "    var total := 0; "
    "    for var i in 0..<200 do "
    "      var t := Program.Fail(); "
    "      try "
    "        await t; "
    "      catch "
    "        var e: ExB => do "
    "          total := total + e.LossAcross(t); "
    "        end end; "
    "    end for; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

}  // namespace

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           local_borrow_survives_task_reraise_spec_31_2_31_1) {
    JitResult r = runAotChecked(kLocalBorrowAcrossReraise);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The borrowed chain's entry, the fresh chain's entry, and
    // 200 * (1 fresh count + 1 borrowed count + 5 = len("first")).
    BEZEL_EXPECT_EQ(out(r), std::string("first\nx\n1400"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           for_over_chain_survives_task_reraise_spec_31_2_31_1) {
    JitResult r = runAotChecked(kForOverChainAcrossReraise);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Both entries, in order, and 200 * (5 + 6): each re-await from inside
    // the arm clears the chain and supersedes nothing (no exception is in
    // flight there), so the fresh chain is empty.
    BEZEL_EXPECT_EQ(out(r), std::string("first\nsecond\n2200"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           local_borrow_survives_append_spec_31_2_30_9) {
    JitResult r = runAotChecked(kLocalBorrowAcrossAppend);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 * (2 = len of the fresh chain's entry 0 + 1 borrowed count +
    // 2 = len("x1")).
    BEZEL_EXPECT_EQ(out(r), std::string("x1\n1000"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           local_borrow_survives_cached_tie_reraise_spec_31_2_22_7) {
    JitResult r = runAotChecked(kCachedTieBorrowAcrossReraise);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 * (0 fresh count + 1 borrowed count + 4 = len("loss")).
    BEZEL_EXPECT_EQ(out(r), std::string("loss\n1000"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           null_conditional_borrow_survives_task_reraise_spec_31_2_31_1) {
    JitResult r = runAotChecked(kNullCondBorrowAcrossReraise);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 * (0 fresh count + 1 borrowed count + 5 = len("first")).
    BEZEL_EXPECT_EQ(out(r), std::string("first\n1200"));
}

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           bare_identifier_borrow_survives_result_reraise_spec_31_2_30_3) {
    JitResult r = runAotChecked(kBareIdentifierBorrowAcrossReraise);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 * (0 fresh count + 1 borrowed count + 5 = len("first")).
    BEZEL_EXPECT_EQ(out(r), std::string("1200"));
}
