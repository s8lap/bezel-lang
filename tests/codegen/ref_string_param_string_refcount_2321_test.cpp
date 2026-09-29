// Regression test for issue #2321 — spec §22.3 / §28.9, v0.236.
//
// A store through a `ref`/`out` parameter of string type is an assignment to
// the caller's location and must follow the ordinary string-slot refcount
// discipline (§4.6 / §28.9): retain the incoming +1 producer into the slot
// and RELEASE the displaced previous string. The ordinary local/field
// `s := s + "!"` statement already does that (lowerStringLocalReassign /
// lowerStringFieldStore); the store through the by-reference parameter
// appeared not to — a plain `CreateStore` through `refParamAddr` left the
// callee-attributed leak.
//
// Mechanical verification of the pre-fix shape: lowering
//   `Rewrite(ref s: string) s := s + "!";` invoked on `var s := "seed" + ""`
// and running the AOT-compiled binary under ASan / LSan reported
//   Direct leak of 32 byte(s) ... in bz$M.Prog$Rewrite
//   Direct leak of 32 byte(s) ... in main
//   Indirect leak of 6 byte(s) ... in bz$M.Prog$Rewrite
//   Indirect leak of 5 byte(s) ... in main
// i.e. 4 allocations, 75 bytes total. The "seed!" +1 freshly produced in
// the callee is allocated there, but the callee also leaves the displaced
// "seed" +1 orphaned in its slot — TWO callee-attributed leaks. After
// this fix (lowerStringRefParamStore in LlvmBackend_Setup.cpp, called from
// the AssignmentStmt fall-through in LlvmBackend_Stmts.cpp), the callee
// emits the load-old / retain-or-transfer-new / store-new / release-old
// pair the spec requires, so the callee-attributed leaks go to zero.
//
// The headline test below exercises this local-`ref s: string` shape and
// pins the fix: it asserts (a) the program's exit code (value correctness)
// and (b) NO leak attributed to the `Rewrite` callee in the ASan output.
// Pre-fix (b) fails — the call stack in the leak report names
// `bz$M.Prog$Rewrite` twice; post-fix (b) passes — the callee is clean.
// The test tolerates a residual caller-side leak in `main` (a separate
// pre-existing escape-walker concern: a local that has been passed as a
// `ref` argument is conservatively not released at scope end; the fix
// targets the callee side, not the caller's slot ownership). The
// `expectAsanClean` shape (leak-tolerance off) would FAIL on the residual
// caller-side leak both pre- and post-fix and so is too coarse; the
// attribution-asserting shape below is the precise signal the fix enables.
//
// Two adversarial tests exercise adjacent ref-param shapes the fix must
// NOT break: an `out` parameter reassignment (the slot's prior contents
// are released in callee) and a `ref` parameter with a literal RHS (no
// +1 to retain — the literal sentinel is a no-op retain/release). Both
// run under the same `Rewrite`-attribution check.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "native_test_harness.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace fs = std::filesystem;

namespace {

// Lower `source` to an object via the real native pipeline; -1 on failure.
int lowerToObject(const std::string& source, const std::string& objPath) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return bezel::test::asanBuildFailure("parse");
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    if (!module) return bezel::test::asanBuildFailure("emitModule");
    std::string err;
    if (!bezel::codegen::emitObjectFile(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    return 0;
}

// Compile + link + run, capturing both exit code and ASan/LSan stderr.
// `crashIsVerifierOnly` treats a non-`WIFEXITED` status (SIGSEGV) as a
// sanitizer-shape failure too — the program must complete the call chain
// and return so we can attribute any leak. Returns -1 on toolchain miss.
//
// #3653: NOT converted to tests/support/run_under_asan.hpp. Materially
// different return TYPE (RunResult{verdict,exitCode,stderrText}, not `int`):
// this pin needs the raw ASan/LSan stderr text to assert leak ATTRIBUTION
// (the callee's stack frame does/doesn't appear in the report), which none
// of the three shared shapes capture.
struct RunResult {
    int verdict;       // 0 = clean, 1 = sanitizer report (leak or crash)
    int exitCode;
    std::string stderrText;
};
RunResult runUnderAsanCaptured(const std::string& source,
                               int expectedExitCode) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return {-1, 0, ""};
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_rstr2321_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();
    if (lowerToObject(source, objPath) != 0) return {-1, 0, ""};
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return {-1, 0, ""}; }
    std::string outPath = exe + ".asan.txt";
    std::string runCmd = "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe +
                         " 2> " + outPath;
    int status = std::system(runCmd.c_str());
    std::ifstream fsOut(outPath);
    std::stringstream ss; ss << fsOut.rdbuf();
    std::string stderrText = ss.str();
    std::remove(outPath.c_str());
    cleanup();
    RunResult r{0, 0, stderrText};
    if (status == -1) return {-1, 0, ""};
    if (!WIFEXITED(status)) {
        r.verdict = 1;
        r.exitCode = 99;
        return r;
    }
    int code = WEXITSTATUS(status);
    r.exitCode = code;
    if (code == 99) { r.verdict = 1; return r; }
    r.verdict = (code == expectedExitCode) ? 0 : 1;
    return r;
}

// True iff `text` contains a `Direct leak` or `Indirect leak` block whose
// allocation-site stack frame names the given function symbol. The ASan
// layout is a one-line header followed by 1+ `#N 0x... in <fn>` frames.
bool stderrMentionsCalleeLeak(const std::string& text,
                              const std::string& calleeSym) {
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (line.find("Direct leak of") == std::string::npos &&
            line.find("Indirect leak of") == std::string::npos) {
            continue;
        }
        for (int i = 0; i < 8 && std::getline(is, line); ++i) {
            if (line.find(" in " + calleeSym) != std::string::npos) return true;
        }
    }
    return false;
}

}  // namespace

// The headline: `method Rewrite(ref s: string) s := s + "!"` invoked on a
// caller-side `var s := "seed" + ""`. Pre-fix: the callee did a plain
// `CreateStore` through the `refParamAddr` pointer, leaving BOTH the
// displaced "seed" +1 and the new "seed!" +1 in callee-attributed leak
// stacks (the "+" allocation site is in the callee; the displaced value
// was orphaned by the bare store). Post-fix: lowerStringRefParamStore
// emits the load-old / retain-or-transfer-new / store-new / release-old
// pair; the callee-attributed leaks go to zero. A residual caller-side
// leak of the new "seed!" in the caller's slot is a separate pre-existing
// escape-walker concern (the local is passed as a `ref` arg, so the
// conservative escape walk does not arm its scope-end release); the
// fix does not target that path, and this test deliberately tolerates it.
BEZEL_TEST(ref_string_param_string_refcount_2321, rewrite_ref_string_param_no_callee_leak) {
    const std::string source =
        "module M; "
        "class Prog "
        "  public static method Rewrite(ref s: string) "
        "    s := s + \"!\"; "
        "  end Rewrite; "
        "  public static method Main(): int "
        "    var s := \"seed\" + \"\"; "
        "    Prog.Rewrite(ref s); "
        "    if s = \"seed!\" then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;";
    RunResult r = runUnderAsanCaptured(source, /*expected=*/0);
    if (r.verdict == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(r.verdict, 0);
    if (r.verdict != 0) return;
    // Pin the fix's specific effect: the callee `Rewrite` MUST NOT appear
    // in the ASan leak report. Pre-fix this assertion fails (the call
    // stacks name `bz$M.Prog$Rewrite` twice — once for the orphaned "seed"
    // displacement, once for the freshly-allocated "seed!" string);
    // post-fix it passes (the callee is clean).
    BEZEL_EXPECT_FALSE(stderrMentionsCalleeLeak(r.stderrText, "bz$M.Prog$Rewrite"));
}

// The `out` sibling: a callee that takes an `out s: string` and assigns
// a literal RHS. The displaced value (slot's prior contents, the literal
// "first" on the second call) is a literal sentinel — the retain-before-
// release helper's null/sentinel safety makes the release a no-op — and
// the new value is a literal sentinel. No callee allocation, no callee
// leak. Pin the value (the second `out` overwrites the first's literal)
// and the absence of a callee leak in the ASan report.
BEZEL_TEST(ref_string_param_string_refcount_2321, out_string_param_reassign_no_callee_leak) {
    const std::string source =
        "module M; "
        "class Prog "
        "  public static method Make(out s: string) "
        "    s := \"first\"; "
        "  end Make; "
        "  public static method Overwrite(out s: string) "
        "    s := \"second\"; "
        "  end Overwrite; "
        "  public static method Main(): int "
        "    var s: string := \"\"; "
        "    Prog.Make(out s); "
        "    Prog.Overwrite(out s); "
        "    if s = \"second\" then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;";
    RunResult r = runUnderAsanCaptured(source, /*expected=*/0);
    if (r.verdict == -1) return;
    BEZEL_EXPECT_EQ(r.verdict, 0);
    if (r.verdict != 0) return;
    BEZEL_EXPECT_FALSE(stderrMentionsCalleeLeak(r.stderrText, "bz$M.Prog$Make"));
    BEZEL_EXPECT_FALSE(stderrMentionsCalleeLeak(r.stderrText, "bz$M.Prog$Overwrite"));
}

// Adversarial: a `ref s: string` callee that assigns a literal RHS — the
// helper's `!isPlusOneStringProducer` gate must NOT re-retain a literal
// sentinel. The displaced "seed" was already a literal (so the in-callee
// `emitStringRelease` of the old value is a no-op on a literal sentinel
// per the runtime's null-safety contract) and the new value is a literal
// sentinel (no retain emitted). Pin: value is correct AND no callee leak
// AND no UAF/DF.
BEZEL_TEST(ref_string_param_string_refcount_2321, rewrite_ref_with_literal_rhs_no_callee_leak) {
    const std::string source =
        "module M; "
        "class Prog "
        "  public static method Rewrite(ref s: string) "
        "    s := \"changed\"; "
        "  end Rewrite; "
        "  public static method Main(): int "
        "    var s: string := \"orig\"; "
        "    Prog.Rewrite(ref s); "
        "    if s = \"changed\" then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end Prog;";
    RunResult r = runUnderAsanCaptured(source, /*expected=*/0);
    if (r.verdict == -1) return;
    BEZEL_EXPECT_EQ(r.verdict, 0);
    if (r.verdict != 0) return;
    BEZEL_EXPECT_FALSE(stderrMentionsCalleeLeak(r.stderrText, "bz$M.Prog$Rewrite"));
}
