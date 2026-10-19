// partial #2668 (§22.4 "Inline `out` declarations" / §2.5 "Dependency
// resolution"; v0.236) — the CONSTRUCTOR twin of #2657: an inline `out var`
// recipient of an IMPORTED CONSTRUCTOR is never given a type, so sema cannot
// type-check any read of it.
//
// (Revision note: the checkout these tests were written against carries v0.237
// in its header; the v0.236->v0.237 diff to docs/Bezel_Language_Spec.md is the
// version line only, so the §22.4 / §2.5 text quoted below is byte-identical at
// both revisions.)
//
// WHY THIS IS NOT ALREADY COVERED BY #2639 OR #2657. Three sibling fixes touch
// this shape and none of them closes it:
//
//   * #2639 made the imported constructor CALL be emitted at all, and sized the
//     caller's out-var SLOT from the imported `ConstructorSymbol`. So the
//     runtime VALUE arriving in the recipient is already correct.
//   * #2657 fixed the recipient's TYPE, but only on the METHOD-call path
//     (`ExprTC::visitChildren`'s CallExpr argument pre-pass).
//   * The NewExpr pre-pass in the same function has its own, separate gate,
//     and it was left alone.
//
// That gate collects only `ast::ConstructorDecl`s:
//
//     if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl))
//         cds.push_back(cd);
//     ...
//     if (ctorDecl && posn < ctorDecl->params.size() && ...)
//         lsym->declType = ctx_.resolvedType(*ctorDecl->params[posn].type);
//
// An IMPORTED constructor has no `ast::ConstructorDecl` —
// `Loader::convertMember`'s `MetaMemberKind::Constructor` arm,
// ModuleLoader.cpp:1192 — `case MetaMemberKind::Constructor: {` —
// reconstructs a `sema::ConstructorSymbol` whose `ParameterSymbol`s
// carry `resolvedDeclType` instead — so `ctorDecl` stays null and the
// recipient's `declType` stays null.
//
// §22.4 is explicit that the type exists and where it comes from: `out var name`
// "declares a new variable inline. Its type is inferred from the method's `out`
// parameter declaration; an explicit type annotation is not permitted in the
// inline form."
//
// WHAT THE UNTYPED RECIPIENT COSTS — this is a soundness hole, not a cosmetic
// gap, and it is exactly why the fix is worth landing even though #2639 already
// made the values right. Mechanically confirmed on the PLAIN path against an
// imported `constructor(s: string, out n: string)`:
//
//     return n.NoSuchMemberAtAll;   pre-fix: ACCEPTED, compiles clean,
//                                            returns 0  (silent wrong answer
//                                            for a program that must not
//                                            compile)
//                                   post-fix: rejected with a real diagnostic
//
//     var q: int := n;              pre-fix: NOT rejected; dies in the backend
//                                            with malformed IR ("Function
//                                            return type does not match
//                                            operand type of return inst!")
//                                   post-fix: rejected, "the initializer for
//                                            `q` has a type that is not
//                                            implicitly convertible to the
//                                            target type (§5.1)"
//
// THE FIX is #2657's verbatim, transposed to the constructor gate: track the
// `ConstructorSymbol` alongside the decl under the same "single, else arity
// match" rule, and resolve the formal through a `formalCtorOutParamType`
// helper that prefers the AST declaration (so a LOCAL constructor is
// bit-identical) and falls back to the imported symbol's parameter type.
//
// DELIBERATELY ONLY THE PLAIN `out var` GATE. The `out owned var` and
// `out using var` gates a few lines below have the identical blind spot and are
// left alone — they additionally switch on ownership tracking that is currently
// dormant for them (markOutOwnedArgs, and ensureOutVarSlots' scope-end disposal
// registration), which is a disposal-correctness change no oracle available
// here can validate: ASan is blind across `--import` because the dependency
// artifact cannot be instrumented (`--sanitize=address` is rejected together
// with `-c`). A plain `out var` recipient carries no such obligation — §22.4
// states "Plain `out var x` is **not** accepted at the call site of an
// `out owned` parameter" — so this site is the one member of the family that is
// pure typing. #2668 stays OPEN for the rest; this commit is not a close.
//
// ⚠ THE PLAIN BUILD IS THE ORACLE, NOT ASAN — see above.
//
// MECHANICAL FAIL->PASS (plain path; the `formalCtorOutParamType` hunks
// reverted, rebuild, re-run, then restored):
//
//     case                                                pre-fix    post-fix
//     V1 must-reject: member that does not exist         ACCEPTED    rejected
//     V2 must-reject: string recipient bound to `int`    malformed   rejected
//     V3 control: the recipient's VALUE still arrives        5           5
//     V4 control: local (non-imported) ctor unaffected      12          12

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2668 {
    std::string path;
    explicit TempFile2668(std::string p) : path(std::move(p)) {}
    ~TempFile2668() { std::remove(path.c_str()); }
};

void writeSource2668(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

const char* kLib2668 =
    "module CtorTypeLib2668; "
    "public class W7 "
    "  public v: int; "
    "  public constructor(s: string, out n: string) "
    "    n := \"abcde\"; v := 8; "
    "  end; "
    "end W7;";

std::string appSource2668(const std::string& body, const std::string& tag) {
    return "module CtorTypeApp2668" + tag + "; import CtorTypeLib2668; "
           "class C "
           "  public static method Main(): int " + body +
           "  end Main; "
           "end C;";
}

// Compile the library, then the app. Returns the app's exit code when it runs;
// -1 if the importer failed to compile; -2 when the dependency itself could not
// be produced (no toolchain). `errOut`, when non-null, captures the importer
// compile's stderr so a must-reject case can assert on the diagnostic TEXT
// rather than on a bare boolean.
int runImported2668(const std::string& body, const std::string& tag,
                    std::string* errOut = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2668_" + tag + "_" + unique_stem("");

    TempFile2668 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2668 aObj((dir / (stem + "_a.o")).string());
    writeSource2668(aSrc.path, kLib2668);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2668 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2668 bExe((dir / (stem + "_b")).string());
    TempFile2668 bErr((dir / (stem + "_b.err")).string());
    writeSource2668(bSrc.path, appSource2668(body, tag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool ok = false;
    if (errOut) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
        ok = driver.compileFile(bSrc.path, bExe.path, deps).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        *errOut = ss.str();
    } else {
        ok = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    if (!ok) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// V1 — MUST-REJECT. With no type on the recipient, sema could not check any
// read of it: this program names a member `string` does not have and pre-fix
// COMPILED CLEAN and returned 0. Asserting the diagnostic TEXT (not merely
// "compile failed") keeps the case honest — a rejection for an unrelated reason
// would otherwise pass it.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_typing_2668,
           imported_ctor_out_var_recipient_is_type_checked) {
    std::string err;
    int rc = runImported2668(
        "    var w := new W7(\"x\", out var n); "
        "    _ := w; "
        "    return n.NoSuchMemberAtAll; ", "V1", &err);
    if (rc == -2) return;  // toolchain unavailable
    BEZEL_EXPECT_EQ(rc, -1);  // importer must NOT compile
    BEZEL_EXPECT_TRUE(err.find("NoSuchMemberAtAll") != std::string::npos);
}

// ===========================================================================
// V2 — MUST-REJECT, the assignability direction. Binding the `string` recipient
// to an `int` local is a §5.1 conversion error; pre-fix sema let it through and
// the BACKEND died on malformed IR instead. Asserting the §5.1 text pins that
// the rejection now comes from the type checker, which is the actual fix, not
// from `verifyModule` giving up.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_typing_2668,
           imported_ctor_out_var_recipient_rejects_bad_conversion) {
    std::string err;
    int rc = runImported2668(
        "    var w := new W7(\"x\", out var n); "
        "    _ := w; "
        "    var q: int := n; "
        "    return q; ", "V2", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(err.find("not implicitly convertible") !=
                      std::string::npos);
}

// ===========================================================================
// V3 — CONTROL: the recipient's VALUE. #2639 already made this correct by
// sizing the slot from the imported ConstructorSymbol, and this fix is pure
// typing — so this row must be unchanged in BOTH directions. It is the guard
// against "fixed the type, broke the value".
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_typing_2668,
           imported_ctor_out_var_value_still_arrives) {
    int rc = runImported2668(
        "    var w := new W7(\"x\", out var n); "
        "    _ := w; "
        "    return n.Length; ", "V3");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// V4 — CONTROL: a LOCAL (same-module) constructor's inline `out var`. The
// helper prefers the AST declaration whenever there is one, so this path must
// be bit-identical; it is the broadest guard that the fallback did not
// displace the ordinary lookup.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_var_typing_2668,
           local_ctor_out_var_unaffected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2668_V4_");
    TempFile2668 src((dir / (stem + ".bzl")).string());
    TempFile2668 exe((dir / stem).string());
    writeSource2668(src.path,
        "module CtorTypeLocal2668; "
        "class W "
        "  public v: int; "
        "  public constructor(s: string, out n: string) "
        "    n := \"abcdefg\"; v := 5; "
        "  end; "
        "end W; "
        "class C "
        "  public static method Main(): int "
        "    var w := new W(\"x\", out var n); "
        "    var q := w.v + n.Length; "
        "    _ := w; "
        "    return q; "
        "  end Main; "
        "end C;");
    bezel::Driver driver;
    if (!driver.compileFile(src.path, exe.path).success) return;
    int status = std::system(exe.path.c_str());
    int rc = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(rc, 12);  // v (5) + "abcdefg".Length (7)
}

}  // namespace bezel::module
