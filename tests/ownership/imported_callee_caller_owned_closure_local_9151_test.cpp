// Regression test for issue #9151 — a caller-owned closure LOCAL passed to an
// IMPORTED VIRTUAL method leaked its closure (16000 B / 600 allocations per
// 200 loops): the escape walk declines an imported callee.
//
// THE DEFECT. `var g := lambda() => r.V; fl.M(g);` — `g` is a LOCAL the caller
// owns: the local's slot holds the +1 the lambda literal minted, and the
// local's own scope-end `bz_closure_release` is the one that must drop it
// (§28.9). Here the class `Fl` is declared in ANOTHER module: the importing
// module sees the callee only as an exported declaration. MEASURED on r30-c1
// @ 6c504f755 with `bezelc --sanitize=address` over the issue's own
// two-module 200-iteration repro:
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
// cells (2 x 8 B).
//
// THE ROOT CAUSE. `ClosureEscapeWalker::expr`'s `CallExpr` arm
// (src/codegen/LlvmBackend_Setup.cpp) asks
// `closureArgBindsMethodOwnTypeParamBorrow` whether the argument is a
// NON-CONSUMING READ (§28.14 call-bounded), and that predicate opened with
// `if (!ms || ms->isOperator || ctx.isImported(ms)) return false;` — an
// IMPORTED callee has no `ast::MethodDecl` here (the loader reconstructs a
// `sema::MethodSymbol` with reconstructed `ParameterSymbol`s and no AST), so
// the premise failed closed, the argument was an ESCAPE,
// `collectClosureReleaseEligible` did not arm the local's scope-end release,
// and the local's own +1 was never dropped.
//
// THE SPEC. §28.14 (v0.359), *The call-bounded default*: "A function-typed
// parameter declared *without* `scoped` is **call-bounded**: the callee
// receives the callback for the duration of the call only ... These obligations
// are compiler-verified for user-authored callees." The obligations were
// verified — in the EXPORTER's module, for the exporter's body (and for every
// override of it). What the importing module cannot see is the DECLARATION,
// and the declaration is exactly what the proof needs: the formal's modifier
// (the exporter copies `ast::Param::modifier` verbatim into
// `MetaParam::modifier`, so `scoped` survives the module boundary — the same
// channel `indexerParamModifier` reads for an imported INDEXER, #9102) and the
// formal's type. §12.1 (v0.359): an override must match the base member's
// parameter types and modifiers and its return type exactly, so the base's
// declaration decides what an override may return — and the RETURN slot is
// where §28.14's one exception (a deferred-activation frame) is announced.
// §28.9 (v0.359): the closure object is runtime-managed, so its own reference
// must be dropped exactly once.
//
// ORACLE. `runAotModulesChecked` — the checked two-module AOT path with the
// AddressSanitizer pass over the GENERATED code and LeakSanitizer at exit.
// Every row asserts the program's own output AND that LeakSanitizer counted
// nothing, so a silent zero-fill cannot pass. The rows LOOP (200 iterations
// inside the program), so the per-call leak cannot hide under the sanitizer's
// noise.
//
// ROWS.
//   Row 1 — the ISSUE'S OWN REPRO: an IMPORTED VIRTUAL member, the
//           caller-owned local passed to it, 200 iterations. RED on the base
//           (16000 B / 600 allocations), GREEN with the fix.
//   Row 2 — the IMPORTED ABSTRACT MEMBER twin: the library declares an
//           abstract class with an abstract member, and the app subclasses it
//           (the override's body is user-authored and §28.14-verified in the
//           app's module). It leaked identically — the importer sees only the
//           declaration, whatever the member's kind. RED on the base (16000 B
//           / 600 allocations), GREEN with the fix.
//
// per §28.14, §28.9, §12.1, §12.5, §2.5 (issue #9151)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotModulesChecked;

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
// and neither AddressSanitizer nor LeakSanitizer fired.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// ── Row 1 (§28.14 / §28.9 / §12.1 / §2.5): the ISSUE'S OWN REPRO — the
// library module declares a VIRTUAL member, the app passes the caller-owned
// closure local to it, 200 iterations.
//
// RED on the base (16000 B / 600 allocations): the escape walk declined the
// imported callee, so the local's own +1 was never dropped. GREEN with the
// fix: the local's scope-end release fires.
BEZEL_TEST(imported_callee_caller_owned_closure_local_9151,
           imported_virtual_member_caller_owned_local_reclaims_the_closure) {
    const std::string lib =
        "module Lib9116; "
        "public class Fl "
        "  public virtual method M(q: () -> int): int => q(); "
        "  public constructor() end constructor; "
        "end Fl;";
    const std::string app =
        "module App9116; "
        "import Lib9116; "
        "import System; "
        "public class Res "
        "  public V: int; "
        "  public constructor() V := 42; end constructor; "
        "end Res; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl := new Lib9116.Fl(); "
        "    for var j in 0..<200 do "
        "      var r := new Res(); "
        "      var g := lambda() => r.V; "
        "      t := t + fl.M(g) + fl.M(g); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotModulesChecked({ModuleSource{"Lib9116", lib},
                                    ModuleSource{"App9116", app}},
                                   "App9116");
    expectClean(r, 0, "16800\n");
}

// ── Row 2 (§28.14 / §28.9 / §12.1 / §12.5 / §2.5): the IMPORTED ABSTRACT
// MEMBER twin — the library declares an abstract class with an abstract
// member, and the app subclasses it (the override's body is user-authored and
// §28.14-verified in the app's own module; §12.1 requires it to match the
// base member's parameter modifiers and return type exactly).
//
// The importer sees only the DECLARATION, whatever the member's kind, so the
// row leaked identically to row 1. RED on the base (16000 B / 600
// allocations), GREEN with the fix.
BEZEL_TEST(imported_callee_caller_owned_closure_local_9151,
           imported_abstract_member_caller_owned_local_reclaims_the_closure) {
    const std::string lib =
        "module LibAbs; "
        "public abstract class Fl "
        "  public abstract method M(q: () -> int): int; "
        "  public constructor() end constructor; "
        "end Fl;";
    const std::string app =
        "module AppAbs; "
        "import LibAbs; "
        "import System; "
        "public class Res "
        "  public V: int; "
        "  public constructor() V := 42; end constructor; "
        "end Res; "
        "public class Sub : LibAbs.Fl "
        "  public constructor() end constructor; "
        "  public override method M(q: () -> int): int => q(); "
        "end Sub; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    var fl: LibAbs.Fl := new Sub(); "
        "    for var j in 0..<200 do "
        "      var r := new Res(); "
        "      var g := lambda() => r.V; "
        "      t := t + fl.M(g); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotModulesChecked({ModuleSource{"LibAbs", lib},
                                    ModuleSource{"AppAbs", app}},
                                   "AppAbs");
    expectClean(r, 0, "8400\n");
}

}  // namespace
