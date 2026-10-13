// Spec-derived regression test for issue #3147 — families F11, F12 and F14 of
// the imported-symbol classification seam (umbrella #3152).
//
// §31.2.4 / §31.4 (F11), §14.4 / §28.5 (F12), §19.3 / §5.1 / §12.9 (F14), all
// against §2.8.3 rule 4 (imported artifacts), v0.252.
// Audit: docs/proposals/papers/imported-symbol-audit-2026-08-13.md, §5.
//
// ── THE SHARED SEAM ────────────────────────────────────────────────────────
//
// `ModuleLoader` never assigns `Symbol::decl` for an imported type; there is no
// `ast::TypeDecl` to assign, because the importing compilation has no AST for a
// dependency's types at all. Every `dynamic_cast<const ast::TypeDecl*>(x->decl)`
// therefore answers "no" for an import, and the guard's `false` branch — written
// as "not an exception class" / "no `owned` field" / "not sealed" — is a
// substantive claim that is silently wrong across a module boundary.
//
// Three one-site residues, each with a named in-tree accessor the site should
// have been calling. Grouped in one commit because each is a single predicate
// and they share one test shape: the SAME program, once with the declaration in
// a dependency and once with it local, must get the SAME answer.
//
// ── MEASURED, on a0b655e, with real two-module builds ──────────────────────
//   (`bezelc -c dep.bzl -o dep.o` then `bezelc --import dep.o -o app app.bzl`)
//
//   F11  `ownershipIsExceptionClassRef` (src/ownership/Ownership_Checks.cpp)
//        walked `td->baseTypes` by name and advanced only through an ancestor
//        that had an `ast::TypeDecl`. Both halves stopped at the boundary.
//          imported `WrapExc: Exception`, `return new WrapExc(ex)` from a catch
//          arm  ->  FALSE REJECT: "caught exception `ex` may not be passed to a
//          constructor of an object that escapes the catch arm (§31.1 / #430)".
//          The same program in one module compiled and ran, exit 0.
//        Fixed by `sema::builtinExceptionBaseOf` (#2950), whose header states
//        the requirement verbatim.
//
//   F12  the §14.4/§28.5 `owned`-bare-type-parameter check in
//        `Resolver::resolveNamed` walked the DECLARING type's AST members, and
//        `tsym` there is the type at a USE site — routinely imported.
//          imported `class HolderO<T> where T: class` with `owned f: T?`,
//          instantiated `HolderO<string>`  ->  ACCEPTED with no diagnostic at
//          any severity. The same declarations in one module produced the §28.5
//          reject. §28.5/§28.9: `string` is runtime-managed and carries no
//          disposal obligation, so it may not bind an `owned` field's parameter.
//
//   F14  `isFinalClass` (src/sema/ExprTypecheck.cpp) hand-scanned the AST
//        modifier list, so an imported `sealed class` read as NON-final and the
//        §19.3/§5.1/§12.9 exhaustiveness reasoning was given up.
//          imported `sealed class BoxF<T>: IBoxF<T>`, pattern
//          `var x: BoxF<string>` against an `IBoxF<int>` scrutinee  ->  MISSED
//          REJECT, compiled clean. The same program in one module was rejected
//          ("its generic instantiation can never match a value of the scrutinee
//          interface type"). Fixed by `typeSymbolHasModifier` /
//          `typeIsAbstractOrClosed` (Symbol.hpp, v0.242/#3052), which is also
//          where the `closed`-subsumes-`abstract` rule is stated once.
//
// ── WHAT EACH CASE PINS ────────────────────────────────────────────────────
//
// Each family gets three cases: the imported program (red before the fix), its
// LOCAL twin (the differential control — green before AND after, so the module
// boundary is proved to be the whole difference), and a NEGATIVE that must not
// move (the check still fires / still does not fire on a shape the fix must not
// have swept in). The F11 negative matters most: `builtinExceptionBaseOf` is a
// broader walk than the hand-rolled one it replaced, so an imported class that
// derives from NOTHING must still be rejected by the #430 check.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile3147 {
    std::string path;
    explicit TempFile3147(std::string p) : path(std::move(p)) {}
    ~TempFile3147() { std::remove(path.c_str()); }
};

void writeSource3147(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The compile's stderr plus its success flag. `depOk` is false when the
// dependency could not be built at all (toolchain unavailable) — the test then
// skips rather than reporting a failure it cannot attribute.
struct Compiled3147 {
    bool depOk = true;
    bool success = false;
    std::string err;
};

// Run one compile with fd 2 redirected to a file, so BOTH the driver's
// std::cerr diagnostics and any llvm::errs() text are captured (the shape
// tests/module/xmod_imported_generic_layout_gate_2821_test.cpp established).
template <typename Fn>
std::string captureStderr3147(Fn&& fn) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz3147_err_" + unique_stem("") + ".txt"))
            .string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

// Build `libSrc` to an object, then compile `appSrc` against it.
Compiled3147 compileImport3147(const std::string& libSrc,
                               const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3147_");
    TempFile3147 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3147 aObj((dir / (stem + "_a.o")).string());
    writeSource3147(aSrc.path, libSrc);
    Compiled3147 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile3147 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3147 bExe((dir / stem).string());
    writeSource3147(bSrc.path, appSrc);
    r.err = captureStderr3147([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        r.success =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    });
    return r;
}

// The one-module twin of the above — same capture, no dependency.
Compiled3147 compileLocal3147(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3147l_");
    TempFile3147 aSrc((dir / (stem + ".bzl")).string());
    TempFile3147 aExe((dir / stem).string());
    writeSource3147(aSrc.path, src);
    Compiled3147 r;
    r.err = captureStderr3147([&] {
        bezel::Driver driver;
        r.success =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                .success;
    });
    return r;
}

bool mentions3147(const Compiled3147& c, const char* needle) {
    return c.err.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// F11 — §31.2.4 / §31.4: an exception class across the module boundary.
// ---------------------------------------------------------------------------
const char* kDepExc3147 =
    "module DepF11_3147; "
    "public class WrapExc3147: Exception "
    "  public constructor(cause: Exception) end constructor; "
    "end WrapExc3147; "
    // The NEGATIVE's subject: derives from NOTHING, so the §31.1/#430 reject
    // must keep firing for it.
    "public class PlainW3147 "
    "  public constructor(e: Exception) end constructor; "
    "end PlainW3147; ";

// The §31.2.4/#811 cause-position shape: a wrapper built from the caught
// exception and returned out of the catch arm.
//
// The return type is spelled `owned`. `Handle` is an INSTANCE method and both
// return expressions are a direct `new` — a fresh owned production. §28.3's
// announcement table row 4 (spec line 6927, v0.307) states that cell exactly:
// "*Instance/extension:* return only a receiver-(parameter-)sourced borrow; a
// fresh owned value is rejected (§28.6) — declare `owned`." The unmarked
// spelling this fixture originally carried was admitted only because the
// row-4 callee-side half was unimplemented; it is now correctly a compile
// error, so the fixture is re-spelled to the form the spec's own cell
// prescribes. The marker is orthogonal to what F11 measures: the §31.1/#430
// catch-escape check runs either way, and T1-vs-T3 (below) still differ ONLY
// in whether the imported wrapper derives from `Exception` — T3 is still
// rejected by #430 and T1 is not, so `ownershipIsExceptionClassRef` stays the
// load-bearing predicate in this fixture.
std::string handlerBody3147(const char* wrapper) {
    std::string w(wrapper);
    return std::string(
        "public class C3147 "
        "  private method DoWork() end DoWork; "
        "  public method Handle(): owned ") + w + " " +
        "    try "
        "      DoWork(); "
        "    catch var ex: Exception => return new " + w + "(ex); "
        "    end; "
        "    return new " + w + "(new Exception()); "
        "  end Handle; "
        "  public static method Main(): int return 0; end Main; "
        "end C3147;";
}

const char* kCatchCtorReject3147 =
    "caught exception `ex` may not be passed to a constructor";

// ---------------------------------------------------------------------------
// F12 — §14.4 / §28.5: the `owned` bare-type-parameter field check.
// ---------------------------------------------------------------------------
const char* kDepOwned3147 =
    "module DepF12_3147; "
    "public class Payload3147 public v: int := 0; end Payload3147; "
    "public class HolderO3147<T> where T: class "
    "  public owned f: T?; "
    "end HolderO3147; ";

const char* kOwnedReject3147 = "may not be instantiated with `string`";

// ---------------------------------------------------------------------------
// F14 — §19.3 / §5.1 / §12.9: finality of an imported `sealed` class.
// ---------------------------------------------------------------------------
const char* kDepSealed3147 =
    "module DepF14_3147; "
    "public interface IBoxF3147<T> end IBoxF3147; "
    "public sealed class BoxF3147<T>: IBoxF3147<T> end BoxF3147; ";

const char* kIfaceInstReject3147 = "scrutinee interface type";

}  // namespace

// ===========================================================================
// F11 / T1 — an imported exception subclass IS an exception class, so the
// §31.2.4 cause-position carve applies and the #430 reject must NOT fire.
// Pre-fix: rejected. // per §31.2.4 / §31.4 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_imported_symbol_residues_3147,
           f11_imported_exception_subclass_is_an_exception_class) {
    auto r = compileImport3147(
        kDepExc3147,
        "module AppF11_3147a; import DepF11_3147; " +
            handlerBody3147("WrapExc3147"));
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_FALSE(mentions3147(r, kCatchCtorReject3147));
    BEZEL_EXPECT_TRUE(r.success);
}

// ===========================================================================
// F11 / T2 — ★ THE DIFFERENTIAL CONTROL. Identical program, exception class
// declared LOCALLY. Green before AND after. // per §31.2.4
// ===========================================================================
BEZEL_TEST(xmod_imported_symbol_residues_3147,
           f11_local_exception_subclass_control) {
    auto r = compileLocal3147(
        "module LocalF11_3147; "
        "public class WrapExc3147L: Exception "
        "  public constructor(cause: Exception) end constructor; "
        "end WrapExc3147L; " +
        handlerBody3147("WrapExc3147L"));
    BEZEL_EXPECT_FALSE(mentions3147(r, kCatchCtorReject3147));
    BEZEL_EXPECT_TRUE(r.success);
}

// ===========================================================================
// F11 / T3 — ★ THE NEGATIVE. `builtinExceptionBaseOf` walks further than the
// predicate it replaced, so this pins that it did not become a blanket accept:
// an imported class deriving from NOTHING is still not an exception class, and
// the §31.1/#430 reject still fires on it. Red-if-over-broad, and it is exactly
// what the audit's "do not partially unify" warning is about (#1462 turned a
// leak into a SIGSEGV by widening one half of a predicate).
// // per §31.1 / #430
// ===========================================================================
BEZEL_TEST(xmod_imported_symbol_residues_3147,
           f11_imported_non_exception_class_still_rejected_negative) {
    auto r = compileImport3147(
        kDepExc3147,
        "module AppF11_3147b; import DepF11_3147; " +
            handlerBody3147("PlainW3147"));
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(mentions3147(r, kCatchCtorReject3147));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// F12 / T1 — §28.5/§28.9: `string` may not bind the type parameter of an
// imported class's `owned T?` field. Pre-fix: ACCEPTED silently.
// // per §14.4 / §28.5 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_imported_symbol_residues_3147,
           f12_imported_owned_bare_type_param_rejects_string) {
    auto r = compileImport3147(
        kDepOwned3147,
        "module AppF12_3147a; import DepF12_3147; "
        "public class Program3147 "
        "  public static method Main(): int "
        "    var h: HolderO3147<string>? := null; "
        "    if h <> null then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end Program3147;");
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(mentions3147(r, kOwnedReject3147));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// F12 / T2 — ★ THE DIFFERENTIAL CONTROL: the same declarations in ONE module
// already produced this reject. // per §28.5
// ===========================================================================
BEZEL_TEST(xmod_imported_symbol_residues_3147,
           f12_local_owned_bare_type_param_rejects_string_control) {
    auto r = compileLocal3147(
        "module LocalF12_3147; "
        "public class HolderO3147L<T> where T: class "
        "  public owned f: T?; "
        "end HolderO3147L; "
        "public class Program3147 "
        "  public static method Main(): int "
        "    var h: HolderO3147L<string>? := null; "
        "    if h <> null then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end Program3147;");
    BEZEL_EXPECT_TRUE(mentions3147(r, kOwnedReject3147));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// F12 / T3 — ★ THE NEGATIVE: a CLASS argument is exactly what §28.5 admits, so
// the newly-reachable check must not over-reject it across the boundary.
// // per §28.5
// ===========================================================================
BEZEL_TEST(xmod_imported_symbol_residues_3147,
           f12_imported_owned_bare_type_param_admits_class_negative) {
    auto r = compileImport3147(
        kDepOwned3147,
        "module AppF12_3147b; import DepF12_3147; "
        "public class Program3147 "
        "  public static method Main(): int "
        "    var h: HolderO3147<Payload3147>? := null; "
        "    if h <> null then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end Program3147;");
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_FALSE(mentions3147(r, kOwnedReject3147));
    BEZEL_EXPECT_TRUE(r.success);
}

// ===========================================================================
// F14 / T1 — an imported `sealed class` is FINAL, so §19.3/§5.1's
// wrong-instantiation reasoning applies and the binding pattern is rejected.
// Pre-fix: compiled clean (a MISSED reject).
// // per §19.3 / §5.1 / §12.9 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_imported_symbol_residues_3147,
           f14_imported_sealed_class_is_final_for_type_test) {
    auto r = compileImport3147(
        kDepSealed3147,
        "module AppF14_3147a; import DepF14_3147; "
        "public class Program3147 "
        "  public method M(b: IBoxF3147<int>): int "
        "    var n := (case b of "
        "      var x: BoxF3147<string> => 1, "
        "      _ => 0 "
        "    ); "
        "    return n; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end Program3147;");
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(mentions3147(r, kIfaceInstReject3147));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// F14 / T2 — ★ THE DIFFERENTIAL CONTROL: the same program in ONE module was
// already rejected. // per §19.3 / §5.1
// ===========================================================================
BEZEL_TEST(xmod_imported_symbol_residues_3147,
           f14_local_sealed_class_is_final_control) {
    auto r = compileLocal3147(
        "module LocalF14_3147; "
        "public interface IBoxF3147L<T> end IBoxF3147L; "
        "public sealed class BoxF3147L<T>: IBoxF3147L<T> end BoxF3147L; "
        "public class Program3147 "
        "  public method M(b: IBoxF3147L<int>): int "
        "    var n := (case b of "
        "      var x: BoxF3147L<string> => 1, "
        "      _ => 0 "
        "    ); "
        "    return n; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end Program3147;");
    BEZEL_EXPECT_TRUE(mentions3147(r, kIfaceInstReject3147));
    BEZEL_EXPECT_FALSE(r.success);
}

// ===========================================================================
// F14 / T3 — ★ THE NEGATIVE: a COMPATIBLE instantiation of the same imported
// sealed class must stay admissible. A finality fix that turned into a blanket
// reject would fail here. // per §19.3 / §12.9
// ===========================================================================
BEZEL_TEST(xmod_imported_symbol_residues_3147,
           f14_imported_sealed_class_compatible_instantiation_negative) {
    auto r = compileImport3147(
        kDepSealed3147,
        "module AppF14_3147b; import DepF14_3147; "
        "public class Program3147 "
        "  public method M(b: IBoxF3147<int>): int "
        "    var n := (case b of "
        "      var x: BoxF3147<int> => 1, "
        "      _ => 0 "
        "    ); "
        "    return n; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end Program3147;");
    if (!r.depOk) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_FALSE(mentions3147(r, kIfaceInstReject3147));
    BEZEL_EXPECT_TRUE(r.success);
}

}  // namespace bezel::module
