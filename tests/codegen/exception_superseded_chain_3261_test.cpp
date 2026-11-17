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

}  // namespace

BEZEL_TEST(codegen_exc_superseded_chain_3261,
           tostring_override_snapshot_is_leak_free_spec_31_2) {
    JitResult r = runAotChecked(kToStringOverrideLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(out(r), std::string("1\nfirst"));
}
