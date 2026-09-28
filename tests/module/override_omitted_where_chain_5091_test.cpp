// Spec-derived regression test for issue #5091 — `materializeConstraintUpChain`
// (the climb that fixes multi-level omitted-where override chains per #5075,
// §14.2 v0.307) matches the ancestor override by SHAPE (full parameter
// signature under buildBaseGenericSubst), not just name + param arity +
// generic arity. A same-shape decoy declared BEFORE the real overloaded
// declaration silently leaks its `where` set onto the leaf override.
//
// spec §14.2 "Constraints on overrides" (v0.307): "An `override` of a generic
// method ... inherits the overridden ... declaration's constraints."
//
// ── ROOT CAUSE (MEASURED) ───────────────────────────────────────────────────
//
// The climb's per-hop ancestor search originally picked a method overload of
// the same name and param arity via the crude predicate:
//   ms && !ms->isOperator && ms->name == cur->name &&
//   ms->params.size() == cur->params.size() &&
//   ms->genericParams.size() == cur->genericParams.size()
// A class that overloaded the name with a same-arity sibling with DIFFERENT
// parameter TYPES (a shape that overload resolution NEVER resolves through,
// because the literal-base sibling mismatch leaves a different method-decl
// available in the AST chain) declared BEFORE the real override was the
// one the climb picked. Its `where T: new()` constraint then flowed onto the
// leaf override's GenericParamSymbol::constraints.
//
// When the body of the leaf override was later code-generated, codegen saw
// `T` was bound ONLY by `new()` (no `IEquatable<T>` constraint on the
// override's own `GenericParamSymbol`) and emitted the warning
//   "internal: codegen cannot dispatch member 'Equals' on receiver of type 'T':
//    permissive acceptance (spec #3 / issue #605) admits this site but no
//    concrete member dispatch exists (lowerLocalDecl's zero-init fallback
//    will degrade the slot — #2477)"
// That warning degrades the slot and breaks `a.Equals(b)` semantically on
// every concrete T (the same #5039-style wrong-dispatch, but as a WARNING,
// not as a compile error). #5039/#5075 fix the silent wrong code; #5091
// fixes the climb's predicate so its reverse-pick stops emitting it.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// The ancestor search now requires Resolver::overrideParamsMatch — the same
// predicate checkHidingAndOverride (line 6536) uses to pick the overridden
// overload — comparing parameter count, modifiers, and types under the same
// buildBaseGenericSubst substitution `checkOverrideSignatureMatch` already
// constructs for that one class-to-base hop. The genericParams.size() guard
// is kept (overrideParamsMatch compares method-level type parameters as
// declaration-less refs). If no candidate is uniquely identified at a hop,
// the climb stops without materializing — preserving the pre-#5075 fall-back.
//
// ── TEST SHAPE (T6) ─────────────────────────────────────────────────────────
//
// Base5091 declares two same-arity generic virtuals with the SAME name and a
// DIFFERENT second parameter type — the decoy (`b: string`, `where T: new()`)
// is declared FIRST; the real overridden declaration (`b: T`, `where T:
// IEquatable<T>`) is declared SECOND. Mid5091 and Leaf5091 override the real
// one and OMIT `where` (the recommended spelling — §14.2).
//
// Pre-fix: the climb picks the decoy on the first hop, copies `new()` onto
// Leaf's `T`, codegen emits the warning above, and the program degrades
// `a.Equals(b)` — the dependency compiles with the warning visible in
// captured stderr.
//
// Post-fix: the climb rejects the decoy by shape mismatch, walks past Mid to
// the real `where T: IEquatable<T>` (or imported side table on cross-
// module), Leaf's `T` is bound by `IEquatable<T>`, no `internal:` warning
// appears, and the dependency compiles cleanly.
//
// Note: the driver does NOT make an external call to `l.M5091<int>(0, 1)`.
// `Base5091` deliberately carries two same-arity generic overloads of `M5091`
// (the decoy + the real override), and as of the #5101 tracking issue, an
// external call to a name with two same-arity generic candidates is rejected
// at overload resolution with a "no overload of `M` is applicable to these
// arguments" diagnostic — a SEPARATE defect from #5091. The #5091 hazard
// surfaces WITHOUT any external call to `M5091` at all: the climb's
// ancestor matching happens at symbol/type resolution time, when
// `Leaf5091.M5091`'s OWN body is checked.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile5091 {
    std::string path;
    explicit TempFile5091(std::string p) : path(std::move(p)) {}
    ~TempFile5091() { std::remove(path.c_str()); }
};

void writeSource5091(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Same-module compile-and-run with stderr capture. The bug surfaces in this
// codepath as a compile-time stderr WARNING (not a compile error); without
// capturing stderr the test would pass vacuously. `runExitCode` is the
// program's exit (92 from Main), or -1 on a compile failure. `compileStderr`
// is whatever the driver printed to stderr while compiling — empty when
// the driver succeeded cleanly (no warnings, no errors).
struct BuildResult5091 {
    int compileExit = -1;   // 0 on driver success, nonzero on driver failure
    int runExitCode = -1;   // -1 on compile failure (no executable)
    std::string compileStderr;
};

template <typename Fn>
std::string captureStderr5091(const std::string& errPath, Fn&& fn) {
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

BuildResult5091 buildLocalAndRun5091(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5091_");
    TempFile5091 srcF((dir / (id + ".bzl")).string());
    TempFile5091 exeF((dir / id).string());
    TempFile5091 errF((dir / (id + "_err.txt")).string());
    writeSource5091(srcF.path, src);

    BuildResult5091 r{};
    r.compileStderr = captureStderr5091(errF.path, [&] {
        bezel::Driver driver;
        auto cr = bezel::test::compileFileForTest(driver, srcF.path, exeF.path);
        if (cr.success) r.compileExit = 0;
    });

    if (r.compileExit == 0) {
        int status = bezel::test::runProgramForTest(exeF.path);
        r.runExitCode =
            (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    }
    return r;
}

const char* kChainBody5091 =
    "public class Base5091 "
    "  public constructor() end constructor; "
    // DECOY FIRST: same name, same arity, same generic arity, but
    // `b: string` (not `b: T`) — a different SHAPE that overload
    // resolution would never pick for an `(a: T, b: T)` call. Its
    // `where T: new()` is what the climb must NOT silently inherit.
    "  public virtual method M5091<T>(a: T, b: string): int where T: new() "
    "    return 63; "
    "  end M5091; "
    // REAL: the actual overridden declaration.
    "  public virtual method M5091<T>(a: T, b: T): int where T: IEquatable<T> "
    "    if a.Equals(b) then return 61; end if; "
    "    return 62; "
    "  end M5091; "
    "end Base5091; "
    "public class Mid5091 : Base5091 "
    "  public constructor() end constructor; "
    "  public override method M5091<T>(a: T, b: T): int "  // omits where
    "    if a.Equals(b) then return 91; end if; "
    "    return 92; "
    "  end M5091; "
    "end Mid5091; "
    "public class Leaf5091 : Mid5091 "
    "  public constructor() end constructor; "
    "  public override method M5091<T>(a: T, b: T): int "  // omits where too
    "    if a.Equals(b) then return 91; end if; "
    "    return 92; "
    "  end M5091; "
    "end Leaf5091; ";

std::string src5091() {
    return std::string("module M5091; ") + kChainBody5091 +
           "class C5091 public static method Main(): int "
           "  return 92; "
           "end Main; end C5091;";
}

}  // namespace

// ===========================================================================
// T6 — ★ THE REPRO. The decoy M5091<T>(a: T, b: string) where T: new()
// declared before the real M5091<T>(a: T, b: T) where T: IEquatable<T> must
// NOT make the climb pick its `new()` constraint as the inherited set. The
// dependency compiles cleanly (compileExit == 0, NO `internal: codegen
// cannot dispatch member 'Equals'` diagnostic in captured stderr), the
// program exits 92.
//
// Pre-fix: the climb picks the decoy, copies `new()` onto Leaf's `T`;
// codegen emits the warning above; the warning is visible in compileStderr.
// (The test was vacuous until stderr was captured — the underlying defect
// is a WARNING, not a compile error, so `compileExit == 0 && runExitCode ==
// 92` was true on BOTH sides of the fix.)
//
// Post-fix: the climb rejects the decoy by shape mismatch, walks past Mid to
// the real `where T: IEquatable<T>`, and the `internal:` warning does not
// appear in compileStderr.
// // per §14.2, v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_chain_5091,
           decoy_sibling_before_real_overload) {
    BuildResult5091 r = buildLocalAndRun5091(src5091());
    BEZEL_EXPECT_EQ(r.compileExit, 0);
    BEZEL_EXPECT_EQ(r.runExitCode, 92);
    if (!r.compileStderr.empty()) {
        bool sawInternalDiagnostic =
            r.compileStderr.find("internal: codegen cannot dispatch") !=
            std::string::npos;
        if (sawInternalDiagnostic) {
            std::fprintf(stderr,
                         "[T6] saw `internal: codegen cannot dispatch` "
                         "diagnostic in compile stderr — climb's predicate "
                         "still picks the decoy. stderr (first 800 chars): "
                         "%.800s\n",
                         r.compileStderr.c_str());
        }
        BEZEL_EXPECT_FALSE(sawInternalDiagnostic);
    }
}

}  // namespace bezel::module
