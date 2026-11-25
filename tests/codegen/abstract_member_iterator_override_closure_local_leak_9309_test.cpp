// Regression test for issue #9309 — a closure LOCAL passed to an ABSTRACT
// member whose override is an ITERATOR (the deferred-activation case) leaked
// its closure: 200 calls leaked 200 closure objects (15200 B).
//
// THE DEFECT. §28.14 (v0.359), *Call-bounded × deferred activation*: "the
// callee receives the callback for the duration of the call only — or, when
// the call mints a deferred-activation frame, for that frame's life". The
// abstract member's return slot announces an ITERATOR's frame, the subclass
// overrides it as an iterator that invokes the callback from its frame, and
// the caller RETURNS the enumerable — so the frame outlives the caller's
// closure local.
//
// The caller's local is a HOLDER of the closure's reference (§28.9: "every
// function-value holder releases exactly one reference"), and the frame is
// another holder: the iterator's frame takes a reference of its OWN on the
// closure cell at construction and drops it at the frame's end (#1864's
// machinery — `LlvmBackend_Iterators.cpp`'s construction retain, and the
// matching release in the frame's `Dispose` / `CloneDispose`). MEASURED on the
// base with `BEZEL_DUMP_IR=1` on the issue's own repro:
//
//   bz$M.Sub$M          (the iterator method)  call void @bz_closure_retain(%iter.clcap)
//   bz$iter$M.Sub$M$Dispose        call void @bz_closure_release(%itc.cl)
//   bz$M.Program$Make  (the CALLER)  *** NO bz_closure_release ***
//
// ⇒ the frame's reference was balanced and the CALLER's was not: the local's
// scope-end release was never armed, so the local's +1 was nobody's to drop.
// MEASURED on the base (r30-c1 @ d668986628), the issue's own 200-iteration
// repro under `bezelc --sanitize=address`:
//
//   Direct leak of 12800 byte(s) in 200 object(s) ...  #1 bz$M.Program$Make
//   Indirect leak of 1600 byte(s) in 200 object(s) ...
//   Indirect leak of 800 byte(s) in 200 object(s) ...
//   SUMMARY: AddressSanitizer: 15200 byte(s) leaked in 600 allocation(s).
//
// 76 bytes per iteration — the closure object (64 B) plus its capture cell
// (8 B) plus the cell's payload (4 B). The program's own output was correct
// (16800); only the memory was not.
//
// THE ROOT CAUSE. #9150's declaration-only proof (the callee-body walk has no
// body to read for an ABSTRACT member, §12.5: an abstract member "declares it
// without a body; every concrete subclass must override it") read the
// callee's DECLARED return slot and DECLINED a deferred-activation return —
// "a Task-family or iterable-interface return (the frame is announced)". Every
// clause failed closed → ESCAPE → the local's scope-end
// `bz_closure_release` was never armed → the leak. The decline was
// conservative: the #9150 note read a frame-announcing return as "the callee
// may hold the callback past the call, so releasing at scope end would be a
// use-after-free while the frame is alive".
//
// THE FIX admits the FRAME-RESIDENT arm: the frame holds a reference of its
// OWN (measured above), so the caller's reference is the caller's to release
// at its scope end, and the closure may OUTLIVE the caller's scope (the frame
// holds it) — so the closure's capture cells stay with its env destructor
// rather than being freed with the scope.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED code and LeakSanitizer at exit. Every row asserts
// the program's own output AND its exit code AND that LeakSanitizer counted
// nothing, so a silent zero-fill cannot pass. The rows LOOP (200 iterations
// inside the program), so the per-call leak cannot hide under the sanitizer's
// noise. Each row also READS the closure after the caller's scope end (the
// frame invokes the callback from its MoveNext), so a fix that released the
// closure too early — before the frame's own reference existed — would fail
// with a heap-use-after-free rather than pass.
//
// ROWS (all RED on the base, GREEN with the fix).
//   Row 1 — the ISSUE'S OWN REPRO: an ABSTRACT member whose override is an
//           ITERATOR, the caller-owned closure local passed to it, 200
//           iterations, the caller RETURNS the enumerable (the frame outlives
//           the local). RED on the base (15200 B / 600 allocations).
//   Row 2 — the INTERFACE MEMBER twin: an interface member has no body either
//           (§11.2: interface members "have no body"; §11.8 declines default
//           interface methods), so the SAME declaration-only proof declined it
//           and the caller-owned local leaked identically. RED on the base.
//   Row 3 — the FRAME-FIRST order: the caller CONSUMES the enumerable inside
//           its own scope, so the frame's disposal runs BEFORE the caller's
//           scope end — the opposite order from row 1. Both orders must be
//           clean, and the base leaked here too. RED on the base.
//
// per §28.14, §28.9, §12.1, §12.5, §11.2, §11.8 (issue #9309)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired. `runAotChecked`
// reports an abort and any sanitizer report as `ran == false`.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// ── Row 1 (§28.14 / §28.9 / §12.1 / §12.5): the ISSUE'S OWN REPRO. An
// ABSTRACT member whose override is an ITERATOR; the receiver's static type is
// the abstract class, so the call binds to the abstract declaration and
// dispatches to the override at runtime. The caller-owned closure local is
// passed to it and the caller RETURNS the enumerable, so the frame outlives
// the local.
//
// RED on the base: 15200 B / 600 allocations leaked over the 200 iterations.
BEZEL_TEST(abstract_member_iterator_override_closure_local_leak_9309,
           abstract_member_with_iterator_override_reclaims_the_closure_local_spec_28_14) {
    const std::string src =
        "module M; "
        "import System; "
        "public closed class Fl "
        "  public abstract method M(q: () -> int): IEnumerable<int>; "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Sub : Fl "
        "  public constructor() end constructor; "
        "  public override method M(q: () -> int): IEnumerable<int> "
        "    yield return q(); "
        "    yield return q(); "
        "  end M; "
        "end Sub; "
        "public class Program "
        "  public static method Make(fl: Fl): IEnumerable<int> "
        "    var c := 42; "
        "    var g := lambda() => c; "
        "    return fl.M(g); "
        "  end Make; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl: Fl := new Sub(); "
        "    for var j in 0..<200 do "
        "      for var x in Make(fl) do t := t + x; end; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    expectClean(r, 0, "16800\n");
}

// ── Row 2 (§28.14 / §28.9 / §11.2 / §11.8): the INTERFACE MEMBER twin. An
// interface member has no body either, so the SAME declaration-only proof
// declined it and the caller-owned local leaked identically. The
// implementation is user-authored and §28.14-verified in its own module, and
// §11.2 requires it to match the interface member's parameter modifiers and
// return type exactly.
//
// RED on the base (15200 B / 600 allocations), GREEN with the fix.
BEZEL_TEST(abstract_member_iterator_override_closure_local_leak_9309,
           interface_member_with_iterator_implementation_reclaims_the_closure_local_spec_28_14) {
    const std::string src =
        "module M; "
        "import System; "
        "public interface IFl "
        "  method M(q: () -> int): IEnumerable<int>; "
        "end IFl; "
        "public class Impl : IFl "
        "  public constructor() end constructor; "
        "  public method M(q: () -> int): IEnumerable<int> "
        "    yield return q(); "
        "    yield return q(); "
        "  end M; "
        "end Impl; "
        "public class Program "
        "  public static method Make(fl: IFl): IEnumerable<int> "
        "    var c := 42; "
        "    var g := lambda() => c; "
        "    return fl.M(g); "
        "  end Make; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl: IFl := new Impl(); "
        "    for var j in 0..<200 do "
        "      for var x in Make(fl) do t := t + x; end; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    expectClean(r, 0, "16800\n");
}

// ── Row 3 (§28.14 / §28.9): the FRAME-FIRST order. Here the caller CONSUMES
// the enumerable INSIDE its own scope, so the frame's disposal runs BEFORE the
// caller's scope end — the opposite order from row 1. Each holder releases
// exactly one reference (§28.9), so whichever order the two releases land in,
// the closure is freed exactly once: no leak, and no double free (which the
// runtime aborts on: "refcount over-release (closure) — two releases for one
// reference", §28.9).
//
// RED on the base (the caller's reference was nobody's to drop), GREEN with
// the fix.
BEZEL_TEST(abstract_member_iterator_override_closure_local_leak_9309,
           frame_disposed_before_the_callers_scope_end_is_clean_spec_28_9) {
    const std::string src =
        "module M; "
        "import System; "
        "public closed class Fl "
        "  public abstract method M(q: () -> int): IEnumerable<int>; "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Sub : Fl "
        "  public constructor() end constructor; "
        "  public override method M(q: () -> int): IEnumerable<int> "
        "    yield return q(); "
        "    yield return q(); "
        "  end M; "
        "end Sub; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl: Fl := new Sub(); "
        "    for var j in 0..<200 do "
        "      var c := 42; "
        "      var g := lambda() => c; "
        "      for var x in fl.M(g) do t := t + x; end; "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    expectClean(r, 0, "16800\n");
}

}  // namespace
