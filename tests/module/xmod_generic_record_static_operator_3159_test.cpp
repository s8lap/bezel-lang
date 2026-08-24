// Cross-module imported user-defined GENERIC-RECORD operator overload — issue
// #3159 (§4.6 / §25 operator resolution, v0.254).
//
// Bug #3159: a `static method operator <token>` declared on a GENERIC record
// resolves & runs when the record is declared in the SAME module, but the
// IMPORTER sets the resolved BinaryExpr's result type to the bare-parametric
// `Vec<T>` (the loader-recorded `importedReturnType`), leaving the concrete
// `Vec<string>` uninstantiated, downstream member access's type stuck on `T`,
// and — for `string + string_thing` uses — a false §4.6 string-concatenation
// diagnostic. The fix mirrors the LOCAL arm's parent-param substitution
// (#1260, `substituteMemberType`) so the imported operator's result
// specializes to the concrete operand instantiation the same way the local
// arm already does.
//
// REGRESSION COVERAGE (three cases, matching the issue's spec'd shape):
//
//   (a) GENERIC imported operator with a PRIMITIVE return type — primary
//       repro, MUST RUN (operator resolves; result type is `int`, not bare
//       `T`; downstream `.v` access / `Console.WriteLine(r)` work).
//       Pre-fix: REJECTED with "`+` requires both operands to be `string`"
//       (§4.6) — completely the wrong diagnostic for a `string + Vec<string>`
//       use; downstream `r.v` typed as bare `T` could not satisfy `string`.
//       Post-fix: compiles, links, prints 7 and exits 7.
//
//   (b) The same operator SAME-MODULE — CONTROL, passes today and after.
//
//   (c) An imported NON-GENERIC record operator — CONTROL (the precedent:
//       this used to be broken #1485 and was the basis for the imported-arm
//       fallback this fix extends), passes today and after.
//
// Note on the issue's RECORD-RETURN repro: that case (§25, `operator +` on a
// generic RECORD with a record return) now COMPILES correctly but reaches
// codegen and trips an IR-verification failure on the call (the exporter's
// generic function was compiled once with the `T = i32` placeholder layout,
// so the importer can't safely call it with `T = string` — this requires
// importer-side per-instantiation specialization, which is a separate,
// larger codegen fix outside the sema scope of #3159). The primitive-return
// case in (a) exercises the same sema path — both reach the same
// `typeBinary` imported arm — and fully proves the fix lands; once
// cross-module operator specialization lands, the record case inherits the
// same sema fix without further change.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

// Per-process unique working dir under the shared scratch base. The previous
// std::filesystem::temp_directory_path() (== /tmp tmpfs) was race-prone even
// with unique_stem: parallel shards racing on the SAME /tmp parent dir
// overwrote each other's _a.bzl / _a.o / _b.bzl mid-compileObject. The
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT). popen() at L72 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir3159() {
    return wtScratchDir("wt-3159");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Run `exe`, capture its stdout, and return {exitCode, stdout}.
std::pair<int, std::string> runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>/dev/null").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// ─── §25 / §4.6 / §14.1 / §2.6 — CORE: GENERIC imported operator, primitive return

// The headline #3159 repro. A generic record `Vec3159<T>` declared in `OpLib3159`
// declares `static method operator +(x: Vec3159<T>, y: Vec3159<T>): T`. The
// consumer declares `var r := p + q` with `p, q: Vec3159<int>` (NO `: int`
// annotation on r — the annotation forced backward inference through the
// BinaryExpr regardless of the import-arm substitution; an unannotated `r`
// propagates the operator's return type as-is, so a pre-fix bug lands at
// `Console.WriteLine(r)` with no WriteLine for `T` generic).
// - pre-fix: `r := p + q` infers `r: T` (the imported operator's unsubstituted
//   return), Console.WriteLine(r) fails to resolve (no `WriteLine<T>`), the
//   consumer executable fails to compile, `rb.success == false`, the test FAILS
//   on `BEZEL_EXPECT_TRUE(rb.success)`.
// - post-fix: `r` is correctly typed `int` (the operator's return substitutes
//   to `int`), the executable compiles, runs, prints 7, exits 7.
BEZEL_TEST(xmod_generic_record_static_operator_3159,
           imported_generic_record_operator_primitive_return_spec_25_14_1) {
    auto dir = wtDir3159();
    std::string stem = unique_stem("bezel_3159_gpr_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module OpLib3159; "
        "public record Vec3159<T> "
        "  public a: T; "
        "  public b: T; "
        "  public static method operator +(x: Vec3159<T>, y: Vec3159<T>): T "
        "    return y.a; "
        "  end; "
        "end Vec3159; ");
    writeSource(bSrc.path,
        "module OpApp3159; "
        "import OpLib3159; "
        "public class C3159 "
        "  public static method Main(): int "
        "    var p := new Vec3159<int>(a := 5, b := 100); "
        "    var q := new Vec3159<int>(a := 7, b := 200); "
        "    var r := p + q; "
        "    Console.WriteLine(r); "
        "    return r mod 256; "
        "  end Main; "
        "end C3159; ");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(bExe.path);
    BEZEL_EXPECT_EQ(code, 7);
    BEZEL_EXPECT_EQ(out, std::string("7\n"));
}

// ─── §25 / §4.6 / §14.1 — CONTROL: same-module twin of (a) ────────────────────

// The byte-identical control: record + operator + Main all in one module.
// Already passes today; the fix MUST NOT regress it. Prints 7 / exits 7.
BEZEL_TEST(xmod_generic_record_static_operator_3159,
           same_module_generic_record_operator_control_spec_25_14_1) {
    auto dir = wtDir3159();
    std::string stem = unique_stem("bezel_3159_gpr_sm_");
    TempFile aSrc((dir / (stem + ".bzl")).string());
    TempFile aExe((dir / (stem + "_a")).string());

    writeSource(aSrc.path,
        "module OpLib3159; "
        "public record Vec3159<T> "
        "  public a: T; "
        "  public b: T; "
        "  public static method operator +(x: Vec3159<T>, y: Vec3159<T>): T "
        "    return y.a; "
        "  end; "
        "end Vec3159; "
        "public class C3159 "
        "  public static method Main(): int "
        "    var p := new Vec3159<int>(a := 5, b := 100); "
        "    var q := new Vec3159<int>(a := 7, b := 200); "
        "    var r: int := p + q; "
        "    Console.WriteLine(r); "
        "    return r mod 256; "
        "  end Main; "
        "end C3159; ");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileFile(aSrc.path, aExe.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    auto [code, out] = runCapture(aExe.path);
    BEZEL_EXPECT_EQ(code, 7);
    BEZEL_EXPECT_EQ(out, std::string("7\n"));
}

// ─── §25 / §9 / §2.6 — CONTROL: imported NON-GENERIC record operator (#1485) ──

// The §25 status quo before #3159: an imported `operator +(Pair,Pair): Pair`
// over a NON-GENERIC record already works (#1485 landed the imported-arm
// result-type fallback this fix extends). This case proves the imported arm
// was the right place to grow the substitution — keeping the fallback
// symmetric, not doubling it. Prints 11 / 22 / exits 11.
BEZEL_TEST(xmod_generic_record_static_operator_3159,
           imported_non_generic_record_operator_control_spec_25_9) {
    auto dir = wtDir3159();
    std::string stem = unique_stem("bezel_3159_ngc_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Op2Lib3159; "
        "public record Pair3159 "
        "  public a: int; "
        "  public b: int; "
        "  public static method operator +(x: Pair3159, y: Pair3159): Pair3159 "
        "    return new Pair3159(a := x.a + y.a, b := x.b + y.b); "
        "  end; "
        "end Pair3159; ");
    writeSource(bSrc.path,
        "module Op2App3159; "
        "import Op2Lib3159; "
        "public class C3159c "
        "  public static method Main(): int "
        "    var p := new Pair3159(a := 1, b := 10); "
        "    var q := new Pair3159(a := 10, b := 12); "
        "    var r := p + q; "
        "    Console.WriteLine(r.a); "
        "    Console.WriteLine(r.b); "
        "    return r.a; "
        "  end Main; "
        "end C3159c; ");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(bExe.path);
    BEZEL_EXPECT_EQ(code, 11);
    BEZEL_EXPECT_EQ(out, std::string("11\n22\n"));
}

// ─── §25 / §14.1 / §9.2 (#3226, v0.254) — RECORD-RETURN imported generic operator
//
// The follow-up to the #3159 family F6 residue: an imported generic RECORD
// operator whose RETURN TYPE IS A RECORD. #3159 fixed the sema-side result-type
// substitution; the codegen-side monomorphization — the importer must
// specialize the operator body per instantiation — was still missing because
// the exporter skipped operator bodies in methodBodyText (only MethodDecl
// arms were carried) and the importer skipped the operator matching arm in
// parseAndAttachMethodBody. With `T = int` (the #3159 pin) the i32-placeholder
// layout happens to equal the per-instantiation layout, so the call passes by
// accident. With `T = string` the per-instantiation layout is `%{ptr, ptr}`
// while the placeholder is `%{i32, i32}`, and the importer-side call reaches
// codegen with mismatched arg/return widths → LLVM IR verifier aborts
// ("Call parameter type does not match function signature"). This case uses
// `T = string` to pin the codegen-side fix.
//
// The fix has three pieces:
//   * ModuleMetadataBuilder.cpp OperatorDecl arm: write the operator's full
//     declaration text into `methodBodyText` on the wire (mirrors the
//     MethodDecl arm exactly).
//   * ModuleLoader.cpp body-attachment loop: remove the `ms->isOperator`
//     skip so the matching walks operators; broaden parseAndAttachMethodBody
//     to attach an OperatorDecl (the type the operator parses to) and to
//     render constructor stubs in the synthetic source (the operator body
//     uses `new Vec3226<T>(a := …, b := …)` which needs a matching ctor
//     declaration to typecheck — records have an implicit positional ctor
//     the metadata does not carry).
//   * LlvmBackend_Calls2.cpp emitMethodCall HARD route: accept OperatorDecl
//     (in addition to MethodDecl) so the imported operator goes through
//     #1059's emitInlineGenericMethod per-instantiation specialization
//     rather than the i32-PLACEHOLDER template.
//
// Expected behavior post-fix:
//   * The consumer compiles cleanly: no IR-verifier abort.
//   * `r.a` reads `"p-a"` (from `x.a`); `r.b` reads `"q-b"` (from `y.b`).
//   * Exits 42 on the OK branch.
BEZEL_TEST(xmod_generic_record_static_operator_3159,
           imported_generic_record_operator_record_return_spec_25_14_1) {
    auto dir = wtDir3159();
    std::string stem = unique_stem("bezel_3226_rr_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module OpLib3226; "
        "public record Vec3226<T> "
        "  public a: T; "
        "  public b: T; "
        "  public static method operator +(x: Vec3226<T>, y: Vec3226<T>): Vec3226<T> "
        "    return new Vec3226<T>(a := x.a, b := y.b); "
        "  end; "
        "end Vec3226; ");
    writeSource(bSrc.path,
        "module OpApp3226; "
        "import OpLib3226; "
        "public class C3226 "
        "  public static method Main(): int "
        "    var p := new Vec3226<string>(a := \"p-a\", b := \"p-b\"); "
        "    var q := new Vec3226<string>(a := \"q-a\", b := \"q-b\"); "
        "    var r := p + q; "
        "    Console.WriteLine(r.a); "
        "    Console.WriteLine(r.b); "
        "    if (r.a = \"p-a\") and (r.b = \"q-b\") then "
        "      Console.WriteLine(\"OK\"); "
        "      return 42; "
        "    else "
        "      Console.WriteLine(\"FAIL\"); "
        "      return 1; "
        "    end; "
        "  end Main; "
        "end C3226; ");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(bExe.path);
    BEZEL_EXPECT_EQ(code, 42);
    BEZEL_EXPECT_EQ(out, std::string("p-a\nq-b\nOK\n"));
}

}  // namespace
}  // namespace bezel::module
