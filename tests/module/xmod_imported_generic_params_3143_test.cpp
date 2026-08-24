// Spec-derived regression test for issue #3143 — family F6 of the
// imported-symbol classification seam (umbrella #3152).
//
// §14.1 (Generic declarations) / §12.9 (Variance) / §9.4 (Records are value
// types) / §2.5 (Dependency resolution) / §2.8.3 rule 4, v0.254. Audit:
// docs/proposals/imported-symbol-audit-2026-08-13.md, family F6.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// Twenty-two sites asked "what are this type's generic parameters?" by casting
// the TypeSymbol's `decl` down to an `ast::TypeDecl` and reading
// `genericParams` off it — for the arity guard, for the parameter NAMES (the
// keys of codegen's `genericSubst_` / `genericSubstSemaTy_` maps), or for the
// §12.9 variance markers.
//
// `ModuleLoader` never assigns `Symbol::decl` for an imported type: there is no
// `ast::TypeDecl` to assign, because the importing compilation has no AST for a
// dependency's types at all. So that cast is null for EVERY imported type, and
// the guard's `false` branch — written as the substantive claim "this type has
// no generic parameters" — was wrong across a module boundary and silent.
//
// The fact is carried on the symbol on both sides: `TypeSymbol::genericParams`,
// written by `SymbolCollection` locally and by
// `ModuleLoader::convertGenericParams` for an import (which sets
// `GenericParamSymbol::name` AND `::variance` from the artifact's
// `MetaGenericParam`). No new field and no metadata change was needed — the fix
// is one accessor, `sema::genericParamNames` / `genericParamVariance`
// (src/sema/GenericParams.hpp), consulted by all twenty-two.
//
// ── MEASURED ON THE PARENT COMMIT (a0b655e), real two-module builds ────────
//   (`bezelc -c lib.bzl -o lib.o`, then `bezelc --import lib.o app.bzl`)
//
//   T1  imported `interface IReadOnly<out T>`, `var x: IReadOnly<Animal> := a;`
//       where `a: IReadOnly<Dog>`
//         -> REJECTED: "the initializer for `x` has a type that is not
//            implicitly convertible to the target type (§5.1)"
//       The byte-identical SINGLE-MODULE program (T4) compiles. Variance read
//       off the absent AST is "", i.e. invariant.
//
//   T2  the same, contravariant `interface IWrite<in T>` -> REJECTED.
//
//   T5  imported `record GRec<T>(rv: T, rtag: int)` named as a consumer method's
//       PARAMETER and RETURN type
//         -> "Function return type does not match operand type of return inst!
//             ret %record.GRec$spec0 ... %record.GRec = type { i32, i32 }"
//            "Call parameter type does not match function signature!"
//            i.e. the erased placeholder layout on one side of the same program
//            and the specialized struct on the other.
//
//   T6  imported `class GBase<T>` with `owned _field: T` and a `T`-typed
//       constructor parameter, consumed as `class Child : GBase<Cell>`
//         -> "Call parameter type does not match function signature!
//             call void @bz$...GBase$ctor(ptr, ptr)" against
//            `bz$...GBase$ctor(ptr, i32)` — the ABI mismatch that
//            emitImplicitBase's substitution block exists to prevent.
//
// ── THE CONTROLS ARE THE POINT ─────────────────────────────────────────────
//
// T3 pins that the predicate is not OVER-broad: an imported INVARIANT
// `interface IInv<T>` must still reject the same assignment — reading variance
// from the symbol must report "" for an unmarked parameter, not "out". T4 and
// T7 are the single-module twins of T1 and T5: both passed before the fix and
// must keep passing, which is what pins that the local answer did not change
// (the accessor reads the AST first when it exists, so a local declaration is
// bit-identical to its pre-fix behaviour).
//
// FAIL-WITHOUT / PASS-WITH was demonstrated mechanically: `git checkout` of the
// ten touched src/ files back to a0b655e, rebuild, T1/T2/T5/T6 fail with exactly
// the diagnostics quoted above while T3/T4/T7 pass; re-apply, all seven pass.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) was race-prone even with
// unique_stem: parallel shards racing on the SAME /tmp parent dir overwrote
// each other's _a.bzl / _a.o / _b.bzl mid-compileObject. The
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT). popen() at L169/214 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir3143() {
    return wtScratchDir("wt-3143");
}

struct TempFile3143 {
    std::string path;
    explicit TempFile3143(std::string p) : path(std::move(p)) {}
    ~TempFile3143() { std::remove(path.c_str()); }
};

void writeSource3143(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Exit codes used by every helper below:
//   -2  the dependency artifact could not be produced (toolchain unavailable) —
//       the test skips, exactly as the other xmod tests do.
//   -1  the IMPORTING side failed to compile/link, or did not exit normally.
// Otherwise the program's own exit status, with its stdout alongside. stdout is
// the value oracle (WEXITSTATUS masks to 8 bits, tests/README.md); the exit
// status additionally carries LeakSanitizer's verdict when `asan` is set.
//
// `errOut`, when non-null, receives the IMPORTING compilation's stderr. Every
// must-REJECT case here asserts the diagnostic TEXT through it, not merely
// `code == -1`: -1 is also what a link failure or an abnormal exit returns, so a
// rejection that moved to an unrelated rule would otherwise pass silently — the
// exact way a control stops controlling anything.
std::pair<int, std::string> compileImportRun3143(const std::string& libSrc,
                                                 const std::string& appSrc,
                                                 const std::string& tag,
                                                 bool asan = false,
                                                 std::string* errOut = nullptr) {
    namespace fs = std::filesystem;
    auto dir = wtDir3143();
    std::string stem = "bz3143_" + tag + "_" + unique_stem("");
    TempFile3143 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3143 aObj((dir / (stem + "_a.o")).string());
    writeSource3143(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile3143 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3143 bExe((dir / (stem + "_b")).string());
    writeSource3143(bSrc.path, appSrc);
    TempFile3143 bErr((dir / (stem + "_b.err")).string());
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        codegen::CodegenOptions opts;
        if (asan) opts.sanitizeAddress = true;
        bool ok = false;
        if (errOut) {
            std::fflush(stderr);
            int saved = ::dup(STDERR_FILENO);
            int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                              0644);
            if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
            ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                 deps, opts).success;
            std::fflush(stderr);
            if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
            std::ifstream in(bErr.path);
            std::ostringstream ss;
            ss << in.rdbuf();
            *errOut = ss.str();
        } else {
            ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                 deps, opts).success;
        }
        if (!ok) return {-1, ""};
    }
    std::string cmd = bExe.path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// The single-module twin, same driver settings — for the T4 / T7 / T8b / T9b
// controls. Same `errOut` contract as above.
std::pair<int, std::string> compileRunLocal3143(const std::string& src,
                                                const std::string& tag,
                                                std::string* errOut = nullptr) {
    namespace fs = std::filesystem;
    auto dir = wtDir3143();
    std::string stem = "bz3143l_" + tag + "_" + unique_stem("");
    TempFile3143 aSrc((dir / (stem + ".bzl")).string());
    TempFile3143 aExe((dir / stem).string());
    TempFile3143 aErr((dir / (stem + ".err")).string());
    writeSource3143(aSrc.path, src);
    bezel::Driver driver;
    bool okLocal = false;
    if (errOut) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(aErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
        okLocal = bezel::test::compileFileForTest(
                      driver, aSrc.path, aExe.path, {},
                      codegen::CodegenOptions{}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(aErr.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        *errOut = ss.str();
    } else {
        okLocal = bezel::test::compileFileForTest(
                      driver, aSrc.path, aExe.path, {},
                      codegen::CodegenOptions{}).success;
    }
    if (!okLocal) return {-1, ""};
    std::string cmd = aExe.path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

bool has3143(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The variance dependency: one covariant interface, one contravariant, one
// unmarked (invariant), and a two-level class chain to convert between.
const char* kVarLib3143 =
    "module VarLib3143; "
    "public interface IReadOnly3143<out T> end IReadOnly3143; "
    "public interface IWrite3143<in T> end IWrite3143; "
    "public interface IInv3143<T> end IInv3143; "
    "public class Animal3143 end Animal3143; "
    "public sealed class Dog3143: Animal3143 end Dog3143;";

// The layout dependency: a generic positional RECORD (T5) and a generic CLASS
// with an owned type-parameter field and a T-typed constructor (T6).
const char* kLayoutLib3143 =
    "module LayLib3143; "
    "public record GRec3143<T>(rv: T, rtag: int); "
    "public class GBase3143<T> "
    "  public owned _field: T; "
    "  public tag: int := 0; "
    "  public constructor(owned f: T) _field := f; end constructor; "
    "  destructor end destructor; "
    "end GBase3143;";

}  // namespace

// ===========================================================================
// T1 — §12.9 COVARIANCE across a module boundary. `IReadOnly3143<Dog>` is
// assignable to `IReadOnly3143<Animal>` because the parameter is declared
// `out T` and `Dog <: Animal`. Pre-fix the variance of an IMPORTED interface
// read as "" (invariant) and the legal assignment was REJECTED — a false
// diagnostic, not a miscompile. // per §12.9 / §2.5, v0.254
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t1_imported_covariant_interface_admits_variant_assignment_12_9) {
    auto [code, out] = compileImportRun3143(
        kVarLib3143,
        "module VarApp3143a; import VarLib3143; "
        "public class C3143 "
        "  public method M(a: IReadOnly3143<Dog3143>) "
        "    var x: IReadOnly3143<Animal3143> := a; "
        "  end M; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"ok1\"); "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t1");
    if (code == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has3143(out, "ok1"));
}

// ===========================================================================
// T2 — the CONTRAVARIANT direction of the same rule: `IWrite3143<Animal>` is
// assignable to `IWrite3143<Dog>` for `in T`. A fix that hard-coded "out" would
// pass T1 and fail here. // per §12.9, v0.254
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t2_imported_contravariant_interface_admits_variant_assignment_12_9) {
    auto [code, out] = compileImportRun3143(
        kVarLib3143,
        "module VarApp3143b; import VarLib3143; "
        "public class C3143 "
        "  public method M(b: IWrite3143<Animal3143>) "
        "    var y: IWrite3143<Dog3143> := b; "
        "  end M; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"ok2\"); "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t2");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has3143(out, "ok2"));
}

// ===========================================================================
// ★ T3 — THE OVER-BROADNESS CONTROL. §12.9: "Type parameters without `out` or
// `in` are invariant." An UNMARKED parameter on an imported interface must
// still reject the same assignment. Reading variance from the symbol must
// report "" for it, not the nearest marked sibling's — this is the assertion
// that a `GenericParamSymbol` with an empty `variance` is distinguished from
// one that is missing. Passes both before and after the fix (before, for the
// wrong reason: everything imported was invariant). // per §12.9, v0.254
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t3_imported_invariant_interface_still_rejects_variant_assignment_12_9) {
    std::string err;
    auto [code, out] = compileImportRun3143(
        kVarLib3143,
        "module VarApp3143c; import VarLib3143; "
        "public class C3143 "
        "  public method M(a: IInv3143<Dog3143>) "
        "    var x: IInv3143<Animal3143> := a; "
        "  end M; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t3", /*asan=*/false, &err);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, -1);  // the importing side must NOT compile
    BEZEL_EXPECT_TRUE(out.empty());
    // ...and for the RIGHT reason. `-1` is also what a link failure or an
    // abnormal exit returns, so the control would survive the rejection moving
    // to some unrelated rule without the text assertion.
    BEZEL_EXPECT_TRUE(has3143(
        err, "is not implicitly convertible to the target type"));
}

// ===========================================================================
// ★ T4 — THE DIFFERENTIAL CONTROL for T1: the byte-identical program with the
// interface declared in the SAME module. It compiled before the fix, which is
// what makes T1's rejection a module-boundary defect rather than a §12.9 one,
// and it must keep compiling after — the accessor reads the AST first, so a
// local declaration answers exactly as it did. // per §12.9
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t4_control_same_module_covariant_assignment_unchanged_12_9) {
    auto [code, out] = compileRunLocal3143(
        "module VarCtl3143; "
        "public interface IReadOnly3143<out T> end IReadOnly3143; "
        "public class Animal3143 end Animal3143; "
        "public sealed class Dog3143: Animal3143 end Dog3143; "
        "public class C3143 "
        "  public method M(a: IReadOnly3143<Dog3143>) "
        "    var x: IReadOnly3143<Animal3143> := a; "
        "  end M; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"ok4\"); "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t4");
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has3143(out, "ok4"));
}

// ===========================================================================
// T5 — §14.1 / §9.4: an IMPORTED generic RECORD named as a consumer method's
// PARAMETER and RETURN type. The annotation must lower to the SAME specialized
// struct the construction and field-access sides produce. Pre-fix the
// annotation fell to the erased `%record.GRec = { i32, i32 }` placeholder while
// the value was `%record.GRec$spec0` — an IR-verification failure, so the
// program did not build at all. The asserted values also pin that the
// specialization is the RIGHT one: `rv` is a `string` field, which at the
// placeholder layout is a truncated i32. // per §14.1 / §9.4 / §2.5, v0.254
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t5_imported_generic_record_as_param_and_return_type_14_1) {
    auto [code, out] = compileImportRun3143(
        kLayoutLib3143,
        "module LayApp3143a; import LayLib3143; "
        "public class C3143 "
        "  public static method Take(r: GRec3143<string>): int "
        "    Console.WriteLine(\"take=\" + r.rv); "
        "    return r.rtag; "
        "  end Take; "
        "  public static method Make(): GRec3143<string> "
        "    return new GRec3143<string>(\"made\", 3); "
        "  end Make; "
        "  public static method Main(): int "
        "    var r: GRec3143<string> := new GRec3143<string>(\"world\", 9); "
        "    Console.WriteLine(\"rv=\" + r.rv); "
        "    var n := Take(r); "
        "    Console.WriteLine(\"n=\" + n.ToString()); "
        "    var m := Make(); "
        "    Console.WriteLine(\"m=\" + m.rv + \"/\" + m.rtag.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t5");
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has3143(out, "rv=world"));
    BEZEL_EXPECT_TRUE(has3143(out, "take=world"));
    BEZEL_EXPECT_TRUE(has3143(out, "n=9"));
    BEZEL_EXPECT_TRUE(has3143(out, "m=made/3"));
}

// ===========================================================================
// T6 — §14.1 / §12 / §28.4: an IMPORTED generic BASE with an `owned _field: T`
// and a `T`-typed constructor parameter, consumed as `class Child : GBase<Cell>`.
// Two F6 sites meet here: emitImplicitBase's base-argument substitution (whose
// absence produced the `bz$GBase$ctor(ptr, ptr)` vs `(ptr, i32)` ABI mismatch
// pre-fix) and emitBaseDisposeChain's specialized base core (whose absence
// leaves the erased template disposer, which cannot classify an erased `T` as
// owned and SKIPS the field — a leak).
//
// Run under generated-code ASan/LSan, so `exit == 0` is also the leak/
// double-free assertion (lane contract §5: never trade a leak for a crash).
// "gone" on stdout is the positive proof the owned base field WAS disposed —
// exactly once, since the destructor prints once per disposal.
// // per §14.1 / §12 / §28.4 / §2.5, v0.254
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t6_imported_generic_base_ctor_and_owned_field_disposal_28_4) {
    auto [code, out] = compileImportRun3143(
        kLayoutLib3143,
        "module LayApp3143b; import LayLib3143; "
        "public class Cell3143 "
        "  public v: int := 0; "
        "  destructor Console.WriteLine(\"gone\"); end destructor; "
        "end Cell3143; "
        "public class Child3143 : GBase3143<Cell3143> "
        "  public own: int := 0; "
        "  public constructor(owned f: Cell3143) base(move f); end constructor; "
        "  destructor end destructor; "
        "end Child3143; "
        "public class C3143 "
        "  public static method Main(): int "
        "    do "
        "      using var c := new Child3143(new Cell3143()); "
        "      c.tag := 7; "
        "      c.own := 9; "
        "      c._field.v := 5; "
        "      Console.WriteLine(\"f=\" + c.tag.ToString() + \"/\" + "
        "        c.own.ToString() + \"/\" + c._field.v.ToString()); "
        "    end; "
        "    Console.WriteLine(\"end\"); "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t6", /*asan=*/true);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has3143(out, "f=7/9/5"));
    BEZEL_EXPECT_TRUE(has3143(out, "gone"));
    BEZEL_EXPECT_TRUE(has3143(out, "end"));
}

// ===========================================================================
// ★ T7 — THE DIFFERENTIAL CONTROL for T5/T6: both shapes in ONE module. They
// built and ran correctly before the fix, so what T5/T6 measure is the module
// boundary and nothing else; and they must keep working after, which pins that
// reading the names through the accessor did not change the local answer.
// // per §14.1 / §9.4 / §28.4
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t7_control_same_module_generic_record_and_base_unchanged_14_1) {
    auto [code, out] = compileRunLocal3143(
        "module LayCtl3143; "
        "public record GRec3143<T>(rv: T, rtag: int); "
        "public class GBase3143<T> "
        "  public owned _field: T; "
        "  public tag: int := 0; "
        "  public constructor(owned f: T) _field := f; end constructor; "
        "  destructor end destructor; "
        "end GBase3143; "
        "public class Cell3143 "
        "  public v: int := 0; "
        "  destructor Console.WriteLine(\"gone\"); end destructor; "
        "end Cell3143; "
        "public class Child3143 : GBase3143<Cell3143> "
        "  public constructor(owned f: Cell3143) base(move f); end constructor; "
        "  destructor end destructor; "
        "end Child3143; "
        "public class C3143 "
        "  public static method Take(r: GRec3143<string>): int "
        "    return r.rtag; "
        "  end Take; "
        "  public static method Main(): int "
        "    var r: GRec3143<string> := new GRec3143<string>(\"world\", 9); "
        "    Console.WriteLine(\"rv=\" + r.rv + \"/\" + Take(r).ToString()); "
        "    do using var c := new Child3143(new Cell3143()); end; "
        "    Console.WriteLine(\"end\"); "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t7");
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(has3143(out, "rv=world/9"));
    BEZEL_EXPECT_TRUE(has3143(out, "gone"));
    BEZEL_EXPECT_TRUE(has3143(out, "end"));
}

// ===========================================================================
// T8 — THE FIRST ACCEPT→REJECT CLASS the removal of
// `typeTestInterfaceBindAmbiguous`'s blanket `!itd -> accept` opens:
// REIFIED + INVARIANT, which reaches path (A) and returns
// `IfaceBindReject::Unreachable` — a `Severity::Error`.
//
// An imported invariant `interface IBox<T>`, scrutinee `IBox<int>`, arm
// `var x: Box3143<string>`. `Box3143<string>` implements only `IBox<string>`,
// which neither converts to nor from `IBox<int>` (§12.9: "Type parameters
// without `out` or `in` are invariant"), so no value of the scrutinee type can
// ever bind the arm — #1913/#1920's dead-arm reject.
//
// Pre-fix this compiled SILENTLY across the boundary while its single-module
// twin (T8b) rejected. §2.8.3 rule 4 is the direct authority: "`is` tests,
// `catch` clauses, and §19 patterns over values of the instantiation match
// identically regardless of which artifact produced the value or performs the
// test." A verdict that differs by artifact is what that forbids.
// // per §19.3 / §12.9 / §2.8.3 rule 4, v0.254
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t8_imported_invariant_iface_dead_arm_now_rejects_19_3) {
    std::string err;
    auto [code, out] = compileImportRun3143(
        "module T8Lib3143; "
        "public interface IBox3143<T> end IBox3143;",
        "module T8App3143; import T8Lib3143; "
        "public class Box3143<T> : IBox3143<T> end Box3143; "
        "public class C3143 "
        "  public method M(s: IBox3143<int>): int "
        "    case s of "
        "      var x: Box3143<string> => do return 1; end, "
        "      _ => do return 0; end "
        "    end; "
        "  end M; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t8", /*asan=*/false, &err);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, -1);
    BEZEL_EXPECT_TRUE(has3143(err, "so the arm is unreachable"));
}

// ===========================================================================
// ★ T8b — its single-module twin. Rejects with the SAME diagnostic, before and
// after the fix. That is what makes T8 a module-boundary defect rather than a
// §19.3 one, and it is the §2.8.2 clause 2 obligation stated as a test: "An
// external instantiation behaves identically to a same-module instantiation of
// the same generic at the same arguments."
// // per §19.3 / §2.8.2 clause 2
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t8b_control_same_module_dead_arm_rejects_both_ways_19_3) {
    std::string err;
    auto [code, out] = compileRunLocal3143(
        "module T8Ctl3143; "
        "public interface IBox3143<T> end IBox3143; "
        "public class Box3143<T> : IBox3143<T> end Box3143; "
        "public class C3143 "
        "  public method M(s: IBox3143<int>): int "
        "    case s of "
        "      var x: Box3143<string> => do return 1; end, "
        "      _ => do return 0; end "
        "    end; "
        "  end M; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t8b", &err);
    BEZEL_EXPECT_EQ(code, -1);
    BEZEL_EXPECT_TRUE(has3143(err, "so the arm is unreachable"));
}

// ===========================================================================
// T9 — THE SECOND ACCEPT→REJECT CLASS: VARIANT + still-ERASED argument, which
// reaches #1941's branch and returns `IfaceBindReject::ErasedAmbiguous`.
//
// An imported covariant `interface ISrc<out T>`, scrutinee
// `ISrc<(string)->string>`, arm `var x: Prod3143<(int)->int>`. A function type
// mints no §14.2 identity token, so the runtime test degrades to the shared
// class tag while §12.9's covariant range admits sibling instantiations as
// values of the scrutinee type — the arm would bind a
// `Prod3143<(string)->string>` value at `Prod3143<(int)->int>`, the #2001/#2006
// memory-unsafe type-confusion class.
//
// Pre-fix, across the boundary, this did not merely accept: the imported
// program reached codegen and aborted with "Basic Block ... does not have
// terminator!" / "IR verification failed". So the change replaces a
// malformed-IR compile abort with the sourced §19.3 diagnostic — strictly
// better, and the same verdict the single-module twin (T9b) has always given.
// // per §19.3 / §12.7 / §2.8.3 rule 4, v0.254
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t9_imported_variant_iface_erased_arg_now_rejects_19_3) {
    std::string err;
    auto [code, out] = compileImportRun3143(
        "module T9Lib3143; "
        "public interface ISrc3143<out T> end ISrc3143;",
        "module T9App3143; import T9Lib3143; "
        "public class Prod3143<T> : ISrc3143<T> end Prod3143; "
        "public class C3143 "
        "  public method M(s: ISrc3143<(string) -> string>): int "
        "    case s of "
        "      var x: Prod3143<(int) -> int> => do return 1; end, "
        "      _ => do return 0; end "
        "    end; "
        "  end M; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t9", /*asan=*/false, &err);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, -1);
    BEZEL_EXPECT_TRUE(has3143(err, "not runtime-distinguishable"));
}

// ===========================================================================
// ★ T9b — its single-module twin, which #1941 has rejected since v0.196. Same
// diagnostic, both ways.
// // per §19.3 / §12.9 / §2.8.2 clause 2
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_params_3143,
           t9b_control_same_module_erased_arg_rejects_both_ways_19_3) {
    std::string err;
    auto [code, out] = compileRunLocal3143(
        "module T9Ctl3143; "
        "public interface ISrc3143<out T> end ISrc3143; "
        "public class Prod3143<T> : ISrc3143<T> end Prod3143; "
        "public class C3143 "
        "  public method M(s: ISrc3143<(string) -> string>): int "
        "    case s of "
        "      var x: Prod3143<(int) -> int> => do return 1; end, "
        "      _ => do return 0; end "
        "    end; "
        "  end M; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C3143;",
        "t9b", &err);
    BEZEL_EXPECT_EQ(code, -1);
    BEZEL_EXPECT_TRUE(has3143(err, "not runtime-distinguishable"));
}

}  // namespace bezel::module
