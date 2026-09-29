// Cross-module axis for fix #4023 (§28.3 K3.4, v0.294) and the consume-side
// ruling v0.303 / #4450 (option 1 of #3975) makes applicable to the
// cross-module shape too.
//
// The single-module tests in checkread_call_arg_move_order_4023_test.cpp
// prove `checkRead`'s `CallExpr` argument loop no longer masks
// `markMovedByName`'s consume-side diagnostic for a `move` argument to a
// callee declared in the SAME module. v0.303 demotes that diagnostic's
// severity for the DOMINATING-kill arm from §28.3 ERROR to §28.2 WARNING —
// the compile no longer fails, but the diagnostic text still prints to
// stderr (warnings are emitted on the same stream). Several other fixes
// this run turned out to be local-axis-only (a predicate that
// dynamic_casts a symbol's `->decl` to an AST node type, which is null for
// every non-generic IMPORTED symbol — see fix #3176's own commentary on
// `recordMovesInArgs`' callee-formals resolver). This file confirms #4023's
// fix does NOT have that shape: the callee (`Factory.Make`) is declared in a
// SEPARATE, imported module, and the disposed local `x` — and the
// masking/skip decision itself — is still correctly resolved.
//
// WHY THIS FIX IS EXPECTED TO BE IMPORT-AGNOSTIC (traced, not just
// measured): the new gate this fix adds,
// `isTopLevelConsumingForm(*a.value)` (Ownership_impl.hpp), inspects ONLY
// the ARGUMENT expression's own AST shape (`dynamic_cast<UnaryExpr*>` with
// `op == "move"`/`"await"`) — it never touches the callee's symbol or
// declaration at all, so whether `Factory.Make` is local or imported cannot
// change whether the skip fires. The diagnostic itself is then emitted by
// `recordMovesInArgs`/`markMovedByName`, pre-existing machinery fix #3176
// already made import-aware (`calleeOrChainFormals` resolves an imported
// method's formals off its `ParameterSymbol` list, not an AST `MethodDecl`).
// This test exercises that combination empirically rather than resting on
// the trace alone.
//
// per §28.2, §28.3 K3.4, §2.5 "Dependency resolution", v0.294 → v0.303,
// #3971, #3941, #4023, #4450

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // compileFileForTest — ASan sweep
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::ownership {
namespace {

namespace fs = std::filesystem;

struct TempFile4023X {
    std::string path;
    explicit TempFile4023X(std::string p) : path(std::move(p)) {}
    ~TempFile4023X() { std::remove(path.c_str()); }
};

void writeSource4023X(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

constexpr char kConsumeSideUseAfterDisposeWarning[] =
    "move from a local `x` already disposed at this point (§28.2)";
// Negative guard: the OLD §28.3 ERROR text must NOT appear in stderr.
// Catches any over-relaxation of the conditional-kill arms (which the
// demotion did NOT touch — see spec §28.2 v0.303 ruling text: "the
// conditional-kill arms stay §28.3 errors per `maybeConsumed`").
constexpr char kOldDisposedMoveError[] =
    "cannot move from disposed local `x` (§28.3)";

// The library module: `Res` (a plain disposable class) and `Factory`, whose
// static `Make` consumes an `owned r: Res` — the IMPORTED callee whose
// argument-list `checkRead` walks through the fixed `CallExpr` arm.
std::string libSource4023X() {
    return
        "module Lib4023X; "
        "public class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "public class Holder "
        "  public v: int; "
        "  public constructor(owned r: Res) this.v := r.tag; r.Dispose(); "
        "end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "public class Factory "
        "  public static method Make(owned r: Res): owned Holder? "
        "    return new Holder(move r); "
        "  end Make; "
        "end Factory;";
}

// The app module: declares a LOCAL `x`, disposes it, and passes `move x` to
// the IMPORTED `Factory.Make` — the issue's own repro shape, across a module
// boundary.
std::string appSource4023X() {
    return
        "module App4023X; "
        "import Lib4023X; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(1); x.Dispose(); "
        "    var y := Factory.Make(move x); "
        "    return y!.v; "
        "  end Main; "
        "end C;";
}

// Compiles the library to an object, then front-end-only compiles the app
// against it (empty outputPath — Driver::compileFile's documented "no
// codegen, no link" path; ownership checking still runs in full, per
// `compileExecutableObject`). Captures the app's stderr for the diagnostic
// text assertion. Returns the app compile's `success`.
bool compileImportedApp4023X(std::string* capturedStderr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4023x_" + unique_stem("");

    TempFile4023X libSrc((dir / (stem + "_lib.bzl")).string());
    TempFile4023X libObj((dir / (stem + "_lib.o")).string());
    writeSource4023X(libSrc.path, libSource4023X());

    // §33.6 "Library artifacts": a dependency object has no entry point, so
    // it is built via `Driver::compileObject` — NOT `compileFile`/
    // `compileFileForTest`, which is the EXECUTABLE path and enforces
    // §33.1/§33.2's `public static method Main` requirement (measured: using
    // `compileFileForTest` here first failed the LIBRARY build itself with
    // "program has no entry point"). `compileObject` is intentionally not
    // wrapped by `driver_run_support.hpp` (see that header's own "WHY
    // compileObject IS NOT WRAPPED" — it does not consult the sanitizer bit
    // at all; ASan coverage of the dependency's allocations still comes from
    // the IMPORTER's executable linking the ASan runtime process-wide).
    bezel::Driver libDriver;
    if (!libDriver.compileObject(libSrc.path, libObj.path).success) {
        // Distinguish a dependency-build failure from the app-under-test
        // failure by aborting loudly here rather than folding both into one
        // false-negative "app rejected" reading.
        if (capturedStderr) *capturedStderr = "<<LIBRARY BUILD FAILED>>";
        return false;
    }
    if (!std::ifstream(libObj.path).good()) {
        if (capturedStderr) *capturedStderr = "<<LIBRARY OBJECT MISSING>>";
        return false;
    }

    TempFile4023X appSrc((dir / (stem + "_app.bzl")).string());
    TempFile4023X appErr((dir / (stem + "_app.err")).string());
    writeSource4023X(appSrc.path, appSource4023X());

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObj.path;
    d.objectPath = libObj.path;
    deps.push_back(d);

    bezel::Driver appDriver;
    bool compiled;
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int sink = ::open(appErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink >= 0) {
        ::dup2(sink, STDERR_FILENO);
        ::close(sink);
    }
    compiled = bezel::test::compileFileForTest(appDriver, appSrc.path,
                                               /*outputPath=*/"", deps)
                   .success;
    std::fflush(stderr);
    if (saved >= 0) {
        ::dup2(saved, STDERR_FILENO);
        ::close(saved);
    }
    if (capturedStderr) {
        std::ifstream in(appErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    }
    return compiled;
}

}  // namespace
}  // namespace bezel::ownership

using bezel::ownership::compileImportedApp4023X;
using bezel::ownership::kConsumeSideUseAfterDisposeWarning;
using bezel::ownership::kOldDisposedMoveError;

// =============================================================================
// NEGATIVE (cross-module) — the issue #4023 repro with the consuming callee
// declared in a SEPARATE, imported module.  v0.303 / #4450 demoted the
// consume-side diagnostic for a DOMINATING-kill arm from §28.3 ERROR to
// §28.2 WARNING (option 1 of #3975), so the front-end's `success` no
// longer trips on this shape — but the WARNING text still prints to stderr
// (warnings share the diagnostic stream).  Pin: the compile succeeds, the
// §28.2 consume-side warning text is in stderr, and the OLD §28.3 ERROR
// text is NOT (negative guard against any over-relaxation of the
// conditional-kill arms, which the demotion did NOT touch).
// =============================================================================
BEZEL_TEST(checkread_call_arg_move_order_4023_xmod,
           imported_callee_bare_call_move_of_disposed_local_warns_28_2_4450) {
    std::string stderrText;
    bool compiled = compileImportedApp4023X(&stderrText);
    BEZEL_EXPECT_TRUE(compiled);  // warnings don't fail the build
    BEZEL_EXPECT_TRUE(stderrText.find(kConsumeSideUseAfterDisposeWarning) !=
                      std::string::npos);
    // Negative guard: OLD §28.3 ERROR text must NOT have re-appeared.
    BEZEL_EXPECT_TRUE(stderrText.find(kOldDisposedMoveError) ==
                      std::string::npos);
}
