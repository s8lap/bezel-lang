// Regression test for issue #9150 — a caller-owned closure LOCAL passed to an
// ABSTRACT method leaked its closure (16000 B / 600 allocations per 200
// iterations): the callee-body walk refuses a callee with no body to read.
//
// THE DEFECT. `var g := lambda() => r.V; fl.M(g);` — `g` is a LOCAL the caller
// owns: the local's slot holds the +1 the lambda literal minted, and the
// local's own scope-end `bz_closure_release` is the one that must drop it
// (§28.9). The callee here is an ABSTRACT member: the receiver's static type is
// the abstract class, so the call binds to the abstract declaration and
// dispatches to the override at runtime. MEASURED on r30-c1 @ 6c504f755 with
// `bezelc --sanitize=address` over the issue's own 200-iteration repro:
//
//   Direct leak of 12800 byte(s) in 200 object(s) allocated from:
//       #1 ... in main ...
//   Indirect leak of 1600 byte(s) in 200 object(s) allocated from:
//       #1 ... in main ...
//   Indirect leak of 1600 byte(s) in 200 object(s) allocated from:
//       #1 ... in main ...
//   SUMMARY: AddressSanitizer: 16000 byte(s) leaked in 600 allocation(s).
//
// 80 bytes per iteration — the closure object (64 B) plus its two capture
// cells (2 x 8 B), for the closure `lambda() => r.V` the issue's own program
// uses (the closure captures `r`).
//
// THE ROOT CAUSE. `ClosureEscapeWalker::expr`'s `CallExpr` arm
// (src/codegen/LlvmBackend_Setup.cpp) asks
// `closureArgBindsMethodOwnTypeParamBorrow` whether the argument is a
// NON-CONSUMING READ (§28.14 call-bounded). For a plain function-typed formal
// it takes arm (b), whose proof is the callee-body walk
// `bodyWalkAllowsForwarding` (src/codegen/LlvmBackend_Calls2.cpp). That walk
// has NO BODY to read for an abstract member (`ast::MethodBodyKind::Abstract`
// → it fails closed), so the premise failed, the argument was an ESCAPE,
// `collectClosureReleaseEligible` did not arm the local's scope-end release,
// and the local's own +1 was never dropped. An INTERFACE member has no body
// either (§11.2: interface members "have no body"; §11.8 declines default
// interface methods), so it failed the same way.
//
// THE SPEC. §28.14 (v0.359), *The call-bounded default*: "A function-typed
// parameter declared *without* `scoped` is **call-bounded**: the callee
// receives the callback for the duration of the call only ... These obligations
// are compiler-verified for user-authored callees." The default is a property of
// the PARAMETER, not of the dispatch: an abstract member's runtime callee is an
// OVERRIDE, and §12.1 (v0.359) requires an override to match the base member's
// parameter TYPES and MODIFIERS and its return type exactly — so the
// override's formal is plain and function-typed too, and the obligations are
// verified for ITS body as well. §12.5 (v0.359): an abstract member "declares
// it without a body; every concrete subclass must override it". §28.9
// (v0.359): the closure object is runtime-managed, so its own reference must be
// dropped exactly once.
//
// ORACLE. `runAotChecked` — the checked AOT path with the AddressSanitizer
// pass over the GENERATED code and LeakSanitizer at exit. Every row asserts
// the program's own output AND that LeakSanitizer counted nothing, so a
// silent zero-fill cannot pass. The rows LOOP (200 iterations inside the
// program), so the per-call leak cannot hide under the sanitizer's noise.
//
// ROWS.
//   Row 1 — the ISSUE'S OWN REPRO: an ABSTRACT member, the caller-owned local
//           passed to it, 200 iterations. RED on the base (16000 B / 600
//           allocations), GREEN with the fix.
//   Row 2 — the INTERFACE MEMBER twin: an interface member has no body either
//           (§11.2 / §11.8), so it failed the walk the same way. RED on the
//           base (16000 B / 600 allocations), GREEN with the fix.
//   Row 3 — GREEN CONTROL: the FRESH LAMBDA at the same abstract callee — a
//           lambda LITERAL written at the argument position, not a local. It
//           is the DOUBLE-RELEASE guard: the literal is released at the call's
//           end by the lowering (`isCallBoundedClosureArgPosition`), so a fix
//           that also armed a release for it would turn this row red with a
//           bad free / use-after-free under ASan.
//   Row 4 — GREEN CONTROL: the DEFERRED-ACTIVATION guard — an abstract member
//           whose return slot announces an iterator's frame, overridden by an
//           iterator that invokes the callback from its frame, with the caller
//           RETURNING the enumerable. §28.14 (v0.359), *Call-bounded ×
//           deferred activation*: the callee "receives the callback ... for
//           that frame's life", so the caller's closure local must NOT be
//           released at its scope end — a fix that admitted this shape would
//           free the closure under the frame and turn this row red with a
//           heap-use-after-free. The row stays LEAK-ONLY (the base's verdict),
//           so it asserts the exact output, a leak floor and NO
//           AddressSanitizer error — never a clean run.
//
// per §28.14, §28.9, §12.1, §12.5, §11.2, §11.8 (issue #9150)

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

// fix #9150: the row stays LEAK-ONLY (the safe direction) and never reads
// freed memory. It compiled, the sanitizer leg really ran, the program printed
// its exact output, LeakSanitizer reported at least `minBytes` leaked, and
// AddressSanitizer reported NO error — a use-after-free aborts the run before
// the leak summary, so it fails both of the last two checks.
void expectLeakFloorNoUseAfterFree(const JitResult& r, const std::string& out,
                                   unsigned long long minBytes) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    std::cout << "    [ LEAK COUNT ] " << leakedAllocs << " allocation(s) / "
              << leakedBytes << " byte(s) leaked (floor " << minBytes
              << " byte(s))\n";
    BEZEL_EXPECT_TRUE(sawSummary);
    BEZEL_EXPECT_TRUE(leakedBytes >= minBytes);
}

// ── Row 1 (§28.14 / §28.9 / §12.1 / §12.5): the ISSUE'S OWN REPRO — an
// ABSTRACT member whose plain function-typed parameter receives the
// CALLER-OWNED local. The receiver's static type is the abstract class, so
// the call binds to the abstract declaration and dispatches to the override at
// runtime. 200 iterations.
//
// RED on the base (16000 B / 600 allocations): the callee-body walk had no
// body to read for an abstract member, so it failed closed and the local's own
// +1 was never dropped. GREEN with the fix: the local's scope-end release
// fires.
BEZEL_TEST(abstract_callee_caller_owned_closure_local_9150,
           abstract_member_caller_owned_local_reclaims_the_closure) {
    const std::string src =
        "module M; "
        "import System; "
        "public class Res "
        "  public V: int; "
        "  public constructor() V := 42; end constructor; "
        "end Res; "
        "public closed class Fl "
        "  public abstract method M(q: () -> int): int; "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Sub : Fl "
        "  public constructor() end constructor; "
        "  public override method M(q: () -> int): int => q(); "
        "end Sub; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl: Fl := new Sub(); "
        "    for var j in 0..<200 do "
        "      var r := new Res(); "
        "      var g := lambda() => r.V; "
        "      t := t + fl.M(g); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    expectClean(r, 0, "8400\n");
}

// ── Row 2 (§28.14 / §28.9 / §11.2 / §11.8): the INTERFACE MEMBER twin — an
// interface member has no body either (§11.2: interface members "have no
// body"; §11.8 declines default interface methods), so the callee-body walk
// failed closed for it exactly as it did for the abstract member, and the
// caller-owned local leaked identically. The implementation is user-authored
// and §28.14-verified in its own module, and §11.2 requires it to match the
// interface member's parameter modifiers and return type exactly.
//
// RED on the base (16000 B / 600 allocations), GREEN with the fix.
BEZEL_TEST(abstract_callee_caller_owned_closure_local_9150,
           interface_member_caller_owned_local_reclaims_the_closure) {
    const std::string src =
        "module M; "
        "import System; "
        "public class Res "
        "  public V: int; "
        "  public constructor() V := 42; end constructor; "
        "end Res; "
        "public interface IFl "
        "  method M(q: () -> int): int; "
        "end IFl; "
        "public class Impl : IFl "
        "  public constructor() end constructor; "
        "  public method M(q: () -> int): int => q(); "
        "end Impl; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl: IFl := new Impl(); "
        "    for var j in 0..<200 do "
        "      var r := new Res(); "
        "      var g := lambda() => r.V; "
        "      t := t + fl.M(g); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    expectClean(r, 0, "8400\n");
}

// ── Row 3 (GREEN CONTROL — the DOUBLE-RELEASE guard): the FRESH LAMBDA at the
// same abstract callee — a lambda LITERAL written at the argument position,
// not a local. The literal is released at the call's end by the lowering
// (`isCallBoundedClosureArgPosition`), so it has no scope-end release of its
// own; a fix that armed one for it as well would free the closure twice and
// turn this row red with a bad free under ASan.
//
// Green on the base (the literal path never went through the escape walk), and
// green with the fix.
BEZEL_TEST(abstract_callee_caller_owned_closure_local_9150,
           fresh_lambda_at_an_abstract_callee_stays_clean) {
    const std::string src =
        "module M; "
        "import System; "
        "public class Res "
        "  public V: int; "
        "  public constructor() V := 42; end constructor; "
        "end Res; "
        "public closed class Fl "
        "  public abstract method M(q: () -> int): int; "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Sub : Fl "
        "  public constructor() end constructor; "
        "  public override method M(q: () -> int): int => q(); "
        "end Sub; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl: Fl := new Sub(); "
        "    for var j in 0..<200 do "
        "      var r := new Res(); "
        "      t := t + fl.M(lambda() => r.V); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    expectClean(r, 0, "8400\n");
}

// ── Row 4 (GREEN CONTROL — the DEFERRED-ACTIVATION guard): the abstract
// member's return slot announces an ITERATOR's frame, the subclass overrides
// it as an iterator that invokes the callback from its frame, and the caller
// RETURNS the enumerable — so the frame outlives the caller's closure local.
//
// §28.14 (v0.359), *Call-bounded × deferred activation*: "the callee receives
// the callback for the duration of the call only — or, when the call mints a
// deferred-activation frame, for that frame's life". The frame is announced by
// the RETURN slot, so the declaration-only proof must decline this callee: a
// fix that admitted it would free the closure at the caller's scope end while
// the frame still holds it, and the frame's `q()` would read the freed closure
// ("AddressSanitizer: heap-use-after-free in lam$0").
//
// The row stays LEAK-ONLY (the base's verdict, and the non-deferred twins'),
// so it asserts the exact output, a leak floor and NO AddressSanitizer error
// — never a clean run.
BEZEL_TEST(abstract_callee_caller_owned_closure_local_9150,
           abstract_member_with_an_iterator_override_never_reads_a_freed_closure) {
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
    expectLeakFloorNoUseAfterFree(r, "16800\n", 200ull * 64ull);
}

}  // namespace
