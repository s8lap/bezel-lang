// Codegen — disposal of the displaced value when an owned class LOCAL is
// reassigned (issues #913, #1054; §28.7 / §17.6).
//
// Reassigning an owned class local must dispose the value it displaces before
// overwriting the slot, else that value leaks:
//   * #1054: plain `c := newOwned` on an owned class local;
//   * #913 (plain-var slice): `c++` ≡ `c := T.operator++(c)` on an owned class
//     local that overloads `operator++`. (The `using var` slice of #913 is a
//     separate sema rejection — `using var` is non-reassignable, §28.10 — so
//     `c++` on a `using var` is a compile error, not a leak.)
//
// SOUNDNESS (the load-bearing controls): the displaced value is disposed ONLY
// when the local definitely OWNS it (an owned plain class local —
// `unwindOwnedLocalClass`). A BORROW receiver — a method parameter (params are
// borrows) — must NOT have its displaced value disposed: that value is owned by
// the caller, and disposing it would be a double-free. `c++` / `c := …` on a
// borrow param therefore leaves the old (borrowed) value alone (the new value
// it creates leaks, which is sound — never a double-free / use-after-free).
//
// Counter's ctor bumps the static counter n (+1), its destructor decrements it
// (-1); `return Counter.n` after a `do…end` scope is the disposed-exactly-once
// balance.
//
// ⚠ RETIRED (the #913 half only) by fix #3978 (§25.1/§17.6, v0.295). §25.1's
// not-overloadable list, verbatim: "The increment and decrement statements
// `++` and `--` ... not operators of the expression grammar." `operator ++`
// is no longer admissible at ALL, so #913's `c++` half of this file's
// premise (the `Counter` shape below with `operator ++`) is now a
// declaration-site compile error, independent of ownership/borrow status.
// `plain_increment_disposes_displaced_913` and
// `borrow_param_increment_no_double_free_913` are FLIPPED to REJECT below.
// The #1054 half (plain `c := new Counter(...)` reassignment, no
// `operator ++` involved at all) is UNCHANGED: `prog()` below had its
// `operator ++` declaration removed (it was never actually exercised by
// `plain_assign_disposes_displaced_1054`, which uses a fresh, non-shared
// Counter class below purely to keep this file's fixture-repair minimal and
// self-contained) and `borrow_param_assign_no_double_free_1054` already
// used its own separate `Counter` class with no `operator ++` at all.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool diagContains913(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_913_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    }
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Counter WITHOUT operator++ (fix #3978 removed it — see file header; this
// helper is used only by #1054's plain-reassignment test, which never
// exercised `c++` at all). ctor +1 / dtor -1.
std::string prog(const std::string& mainBody) {
    return
        "module M; "
        "class Counter "
        "  public static n: int := 0; "
        "  public Value: int; "
        "  public constructor(v: int) n := n + 1; Value := v; end constructor; "
        "  destructor n := n - 1; end destructor; "
        "end Counter; "
        "class C public static method Main(): int " + mainBody + " end Main; end C;";
}

// #1054: `c := new Counter(5)` on an owned plain class local disposes the
// displaced Counter(0). After reassignment (before the final discharge) only
// Counter(5) is alive → n == 1.
BEZEL_TEST(owned_local_reassign_913_1054, plain_assign_disposes_displaced_1054) {
    expectDisposedAndLeakFree(
        prog("do var c := new Counter(0); c := new Counter(5); "
             "var m := Counter.n; c.Dispose(); return m; end;"), 1);
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "#913 (plain-var slice): `c++` on an owned plain class local
// disposes the displaced Counter(0); only Counter(1) is alive after →
// n == 1." `operator ++` is no longer admissible on any declaring type
// (§25.1) — the declaration itself is now rejected, independent of the
// ownership-disposal machinery this row used to exercise.
BEZEL_TEST(owned_local_reassign_913_1054,
           plain_increment_declaration_rejected_spec_25_1_issue_3978) {
    auto r = runAot(
        "module M; "
        "class Counter "
        "  public static n: int := 0; "
        "  public Value: int; "
        "  public constructor(v: int) n := n + 1; Value := v; end constructor; "
        "  public static method operator ++(c: Counter): owned Counter => "
        "    new Counter(c.Value + 1); "
        "  destructor n := n - 1; end destructor; "
        "end Counter; "
        "class C public static method Main(): int "
        "  do var c := new Counter(0); c++; "
        "  var m := Counter.n; c.Dispose(); return m; end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains913(r, "operator `++` may not be overloaded (§25.1)"));
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "CRITICAL CONTROL: `c++` on a BORROW param must NOT dispose the
// displaced (borrowed) value ... the load-bearing assertion is NO
// double-free / use-after-free." That double-free question is moot once
// `operator ++` cannot be declared at all — the program is now rejected at
// the declaration, before any borrow/ownership question is reachable.
BEZEL_TEST(owned_local_reassign_913_1054,
           borrow_param_increment_declaration_rejected_spec_25_1_issue_3978) {
    std::string src =
        "module M; "
        "class Counter public static n: int := 0; public Value: int; "
        "  public constructor(v: int) n := n + 1; Value := v; end constructor; "
        "  public static method operator ++(c: Counter): owned Counter => "
        "    new Counter(c.Value + 1); "
        "  destructor n := n - 1; end destructor; "
        "end Counter; "
        "class C "
        "  public static method Bump(c: Counter): int c++; return c.Value; end Bump; "
        "  public static method Main(): int "
        "    using var c := new Counter(10); return Bump(c); end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains913(r, "operator `++` may not be overloaded (§25.1)"));
}

// CONTROL: `c := newOwned` on a BORROW param likewise must not double-free the
// borrowed value.
BEZEL_TEST(owned_local_reassign_913_1054, borrow_param_assign_no_double_free_1054) {
    std::string src =
        "module M; "
        "class Counter public static n: int := 0; public Value: int; "
        "  public constructor(v: int) n := n + 1; Value := v; end constructor; "
        "  destructor n := n - 1; end destructor; "
        "end Counter; "
        "class C "
        "  public static method Repl(c: Counter): int c := new Counter(50); "
        "    return c.Value - 43; end Repl; "  // 7, not the ASan exitcode sentinel 99
        "  public static method Main(): int "
        "    using var c := new Counter(1); return Repl(c); end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
    int lsan = runUnderLeakSanitizer(src, /*detectLeaks=*/false);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);  // no double-free / use-after-free
}

}  // namespace
