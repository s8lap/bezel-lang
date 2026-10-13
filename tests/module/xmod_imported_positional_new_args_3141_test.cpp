// Spec-derived regression test for issue #3141 — family F3 of the
// imported-symbol classification seam (umbrella #3152).
//
// §9.1 (positional records) / §22.5 (argument binding) / §5.1 (implicit
// conversions) / §22.10 (primary constructors) / §22.8 (construction sequence)
// / §14.2 (`where T: new()`) / §9.7 (borrow-bearing records) / §2.8.3 rule 4
// (imported artifacts), v0.254. Audit:
// docs/proposals/papers/imported-symbol-audit-2026-08-13.md, family F3.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// Sixteen sites answered "what are this type's positional (primary-constructor)
// parameters?" by casting `Symbol::decl` to an `ast::TypeDecl`. `ModuleLoader`
// never assigns `Symbol::decl` for an imported type — there is no
// `ast::TypeDecl` to assign, since the importing compilation has no AST for a
// dependency's types — so every one of them answered "no positional fields" for
// every import, silently and with no diagnostic.
//
// The worst consequence was at `ExprTC::checkNewExprArgs`: #1105's argument
// check (added because codegen "stored the 8-byte string pointer into the
// 4-byte `tag` field, corrupting the stack") never ran across a module
// boundary.
//
// ── MEASURED, on `a9a2684`, with a real two-module build ───────────────────
//   (`bezelc -c lib.bzl -o lib.o` then `bezelc --import lib.o app.bzl`)
//
//   For `public record Pt(X: int, Y: int); public record Named(Tag: string, N: int);`
//
//   | program (identical apart from where the type is declared) | LOCAL | IMPORTED (pre-fix) |
//   |---|---|---|
//   | `new Pt(1)`               | rejected §22.5             | compiled; ran; `p.X=0 p.Y=1`  |
//   | `new Pt("hello", 2)`      | rejected §5.1              | compiled; ran; `p.X=4434336`  |
//   | `new Named(7, 2)`         | rejected §5.1              | compiled; **SIGSEGV**         |
//   | `new Wide(3000000000, 5)` | ran, `u=3000000000 l=5`    | REJECTED ("does not fit in `int`") |
//   | `class D(): PBox(5, 6)`   | ran, `v=5 w=6`             | ran, `v=0 w=0`                |
//   | `h: Holder := new Holder(null, 7)` | ran, exit 0       | REJECTED (§9.7)               |
//   | `new Holder<RefRec>()`    | rejected §14.2             | compiled                      |
//
// Three distinct failure modes in one family: memory unsafety (rows 1–3), silent
// wrong data (row 5), and FALSE REJECTS (rows 4, 6) — the last two of which the
// §9.7 half only acquired when #3145 (family F10) correctly started answering
// "borrow-bearing" for imports while this half still answered "not null-source".
//
// ── HOW IT IS FIXED ────────────────────────────────────────────────────────
//
// Every site reads the symbol table, which carries the list on both sides:
// `TypeSymbol::hasPositionalFields` is mirrored (#501) and a positional field is
// a `FieldSymbol` locally (`posParam` -> the real `ast::Param`) and on import
// too (#2516, a synthetic `ast::Param` with a null `type`). The accessors are
// `sema::recordPositionalFieldSymbols` (the indexed view) and
// `sema::storageComponentType` (the declared type, consulting both sources),
// both landed by #3145.
//
// ── WHAT THIS FILE DOES NOT COVER ──────────────────────────────────────────
//
// `new ImportedClass(args)` through a §22.10 primary constructor stays rejected
// — the parameters have no channel across the boundary at all (#3162). T9 pins
// that the rejection now names the real reason instead of an implicit default
// constructor the class does not have.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile3141 {
    std::string path;
    explicit TempFile3141(std::string p) : path(std::move(p)) {}
    ~TempFile3141() { std::remove(path.c_str()); }
};

void writeSource3141(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// `dependencyCompiled` is the ANTI-VACUITY guard: without it a malformed lib
// source would turn an expected importer rejection into a pass for the wrong
// reason (nothing imported, so nothing to check).
struct ImportCapture3141 {
    bool dependencyCompiled = false;
    bool appCompiled = false;
    std::string error;
};

std::string captureStderrOf(const std::function<void()>& body) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz3141_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    body();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return err;
}

// Two-module build, importer stderr captured. Used by the REJECTION tests.
ImportCapture3141 compileImport3141(const std::string& libSrc,
                                    const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3141_");
    TempFile3141 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3141 aObj((dir / (stem + "_a.o")).string());
    writeSource3141(aSrc.path, libSrc);
    ImportCapture3141 out;
    {
        bezel::Driver driver;
        out.dependencyCompiled = driver.compileObject(aSrc.path, aObj.path).success &&
                                 std::ifstream(aObj.path).good();
    }
    if (!out.dependencyCompiled) return out;
    TempFile3141 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3141 bExe((dir / stem).string());
    writeSource3141(bSrc.path, appSrc);
    out.error = captureStderrOf([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        codegen::CodegenOptions opts;
        out.appCompiled =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps,
                                            opts).success;
    });
    return out;
}

// Two-module build + RUN, generated-code ASan on. -2 = the dependency did not
// build (test would be vacuous); -1 = the app did not build or did not exit
// normally; otherwise the program's exit code. Used by the ACCEPT tests, so
// each of them pins a VALUE, not merely "it compiled".
int compileImportAndRun3141(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3141r_");
    TempFile3141 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3141 aObj((dir / (stem + "_a.o")).string());
    writeSource3141(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile3141 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3141 bExe((dir / stem).string());
    writeSource3141(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;  // LSan forces exit 1 on a leak
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Same-module build, stderr captured — the LOCAL twin of a rejection test.
std::string compileLocal3141(const std::string& src, bool& ok) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3141l_");
    TempFile3141 srcFile((dir / (stem + ".bzl")).string());
    TempFile3141 exeFile((dir / stem).string());
    writeSource3141(srcFile.path, src);
    bool success = false;
    std::string err = captureStderrOf([&] {
        bezel::Driver driver;
        codegen::CodegenOptions opts;
        success = bezel::test::compileFileForTest(driver, srcFile.path,
                                                  exeFile.path, {}, opts)
                      .success;
    });
    ok = success;
    return err;
}

// Same-module build + RUN — the LOCAL twin of an accept test.
int compileLocalAndRun3141(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3141lr_");
    TempFile3141 srcFile((dir / (stem + ".bzl")).string());
    TempFile3141 exeFile((dir / stem).string());
    writeSource3141(srcFile.path, src);
    bezel::Driver driver;
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, srcFile.path, exeFile.path, {},
                                         opts)
             .success)
        return -1;
    int status = std::system(exeFile.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The dependency every test imports. `GBox3141` is the §14.1 / #428-phase-B
// shape: an imported GENERIC record, whose `V: T` field must be checked against
// the instantiation's own argument rather than against the bare parameter.
//
// Per spec §22.10 (v0.307, line 1861) post-#4868 5458a4e1: the CLASS `PBox3141`
// primary-ctor parameters are NOT auto-promoted to public properties. Records
// (§9.1) auto-promote positional fields, so `record GBox3141<T>(V: T, N: int)`
// keeps its public `V`/`N` surface; the migration renames `PBox3141`'s params
// to camelCase (the canonical §22.10 idiom, docs/Bezel_Language_Spec.md:2042-
// 2057) and adds explicit `public readonly property` PascalCase accessors so
// external reads (`d.V`, `d.W`) still work. In-class body reads (`v + w` in
// `Sum()`) still resolve to the captured primary-ctor fields per §22.10's
// initializer-vs-body distinction (line 1859).
const char* kLib =
    "module DepF3_3141; "
    "public record Pt3141(X: int, Y: int); "
    "public record Named3141(Tag: string, N: int); "
    "public record Wide3141(U: uint, L: long); "
    "public record RefRec3141(R: Object, N: int); "
    "public record GBox3141<T>(V: T, N: int); "
    "public class PBox3141(v: int, w: int) "
    "  public readonly property V: int := v; "
    "  public readonly property W: int := w; "
    "  public method Sum(): int return v + w; end Sum; "
    "end PBox3141; ";

// The §9.7 dependency — a record with a NULLABLE borrow-carrying field, which
// is the shape §9.7's null-source exemption is written for.
const char* kLibNull =
    "module DepF3N_3141; "
    "public class Res3141 "
    "  public v: int := 0; "
    "  public constructor(x: int) v := x; end constructor; "
    "end Res3141; "
    "public record Holder3141(r: Res3141?, tag: int); ";

}  // namespace

// ===========================================================================
// ★ T1 — THE HEADLINE. `new Named3141(7, 2)` binds an `int` argument to a
// `string` field of an IMPORTED record. Pre-fix this compiled clean and the
// program SIGSEGV'd on first use of the field (an integer read back as a string
// pointer). It must be rejected at compile time, by §5.1, exactly as the local
// twin (T1b) already was. // per §5.1 / §9.1 / §2.8.3 rule 4, v0.254
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_wrong_typed_arg_is_rejected) {
    auto r = compileImport3141(
        kLib,
        "module AppF3_3141a; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var p := new Named3141(7, 2); "
        "    return p.N; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);   // anti-vacuity
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(has(r.error, "the argument `Tag` to `new Named3141`"));
    BEZEL_EXPECT_TRUE(has(r.error, "(§5.1)"));
}

// ===========================================================================
// ★ T1b — THE DIFFERENTIAL CONTROL for T1. Byte-identical program with the
// record declared in the SAME module. It rejected before the fix and must still
// reject, with the SAME diagnostic: that is what pins that the module boundary
// was the entire difference, and that moving the walk to the symbol table did
// not change the local answer. // per §5.1 / §9.1
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           local_record_wrong_typed_arg_still_rejected_control) {
    bool ok = true;
    std::string err = compileLocal3141(
        "module LocF3_3141a; "
        "public record Named3141(Tag: string, N: int); "
        "public class C "
        "  public static method Main(): int "
        "    var p := new Named3141(7, 2); "
        "    return p.N; "
        "  end Main; "
        "end C;", ok);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(has(err, "the argument `Tag` to `new Named3141`"));
    BEZEL_EXPECT_TRUE(has(err, "(§5.1)"));
}

// ===========================================================================
// T2 — arity. `new Pt3141(1)` for a two-field imported record. Pre-fix it
// compiled and ran, storing the sole argument into the SECOND field: the
// program printed `p.X=0 p.Y=1`. §22.5's "required parameter is not supplied"
// is the local answer and must now be the imported one too.
// // per §22.5 / §9.1 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_missing_arg_is_rejected) {
    auto r = compileImport3141(
        kLib,
        "module AppF3_3141b; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var p := new Pt3141(1); "
        "    return p.X; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(
        has(r.error, "`new Pt3141`: required parameter `Y` is not supplied"));
    BEZEL_EXPECT_TRUE(has(r.error, "(§22.5)"));
}

// ===========================================================================
// T2b — the local twin of T2, same wording. // per §22.5 / §9.1
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           local_record_missing_arg_still_rejected_control) {
    bool ok = true;
    std::string err = compileLocal3141(
        "module LocF3_3141b; "
        "public record Pt3141(X: int, Y: int); "
        "public class C "
        "  public static method Main(): int "
        "    var p := new Pt3141(1); "
        "    return p.X; "
        "  end Main; "
        "end C;", ok);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(
        has(err, "`new Pt3141`: required parameter `Y` is not supplied"));
}

// ===========================================================================
// ★ T3 — THE FALSE-REJECT CONTROL, and the reason this file is not simply
// "everything is rejected now". A well-formed `new Pt3141(3, 4)` must still
// compile AND produce the right values. Returns X * 10 + Y = 34, so a store
// that landed in the wrong slot (the pre-fix behaviour for a mis-arity call)
// would show as a different exit code, not merely as "it compiled".
// // per §9.1 / §22.5
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_wellformed_construction_still_runs) {
    BEZEL_EXPECT_EQ(compileImportAndRun3141(
        kLib,
        "module AppF3_3141c; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var p := new Pt3141(3, 4); "
        "    return p.X * 10 + p.Y; "
        "  end Main; "
        "end C;"), 34);
}

// ===========================================================================
// T4 — §22.5 NAMED arguments, reordered, on an imported record. This is what
// proves the symbol-side binder binds by FIELD NAME and keeps field i paired
// with the argument that names it: a positional-order binder would return 43.
// // per §22.5 ("a named argument fills the parameter whose name matches")
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_named_args_bind_by_field_name) {
    BEZEL_EXPECT_EQ(compileImportAndRun3141(
        kLib,
        "module AppF3_3141d; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var p := new Pt3141(Y := 4, X := 3); "
        "    return p.X * 10 + p.Y; "
        "  end Main; "
        "end C;"), 34);
}

// ===========================================================================
// T4b — a named argument that matches no field must be rejected by §22.5, not
// silently dropped. Pins that the symbol-side binder carries the whole rule and
// not just its arity half. // per §22.5
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_unknown_named_arg_is_rejected) {
    auto r = compileImport3141(
        kLib,
        "module AppF3_3141e; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var p := new Pt3141(X := 3, Z := 4); "
        "    return p.X; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(has(r.error, "has no parameter named `Z`"));
    BEZEL_EXPECT_TRUE(has(r.error, "(§22.5)"));
}

// ===========================================================================
// ★ T5 — §5.1 literal widening, the SECOND false reject. The per-argument
// literal-type hint (`currentLiteralWideningHint_`) was derived from the
// positional field's declared type through the same dead AST cast, so an
// imported `record Wide3141(U: uint, L: long)` gave NO hint and
// `new Wide3141(3000000000, 5)` was rejected — "integer literal does not fit in
// `int`" — while the identical local record accepted it. Both fields are read
// back, so a lost hint shows as a compile failure (-1) and a value that did not
// survive the widening shows as 91/92 rather than 6.
// // per §5.1 / §9.1 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_literal_widens_to_positional_field_type) {
    BEZEL_EXPECT_EQ(compileImportAndRun3141(
        kLib,
        "module AppF3_3141f; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var w := new Wide3141(3000000000, 5); "
        // The `U` suffix is on the COMPARISON literal only: an unsuffixed
        // literal in a comparison has no positional-field target to widen
        // against, which is a separate §5 rule and not what this pins. The
        // CONSTRUCTION argument is deliberately unsuffixed — the hint is the
        // whole point.
        "    if w.U <> 3000000000U then return 91; end; "
        "    if w.L <> 5 then return 92; end; "
        "    return 6; "
        "  end Main; "
        "end C;"), 6);
}

// ===========================================================================
// ★ T6 — §22.8 step 0 across the boundary. A LOCAL class whose header clause
// feeds an IMPORTED primary-constructor base (`class D(): PBox3141(5, 6)`).
// `prePopulateBasePositionalFields` gated the whole pre-population on the
// BASE's `ast::TypeDecl`, so for an imported base nothing was stored and the
// base subobject stayed calloc-zero: measured `v=0 w=0` (and `Sum() = 0`)
// against `v=5 w=6` for the byte-identical local base. Silent wrong data — it
// compiled and ran clean both times. Returns Sum() = 11.
// // per §22.8 step 0 / §11.1 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_primary_ctor_base_positional_fields_are_populated) {
    BEZEL_EXPECT_EQ(compileImportAndRun3141(
        kLib,
        "module AppF3_3141g; import DepF3_3141; "
        "public class D3141(): PBox3141(5, 6) "
        "  public method Twice(): int return Sum() * 2; end Twice; "
        "end D3141; "
        "public class C "
        "  public static method Main(): int "
        "    var d := new D3141(); "
        "    if d.V <> 5 then return 91; end; "
        "    if d.W <> 6 then return 92; end; "
        "    return d.Twice() - 11; "
        "  end Main; "
        "end C;"), 11);
}

// ===========================================================================
// T6b — the local twin of T6: it ran correctly before the fix and must still.
// // per §22.8 step 0 / §11.1
// ===========================================================================
// T6b — the local twin of T6: it ran correctly before the fix and must still.
// // per §22.8 step 0 / §11.1
//
// Per spec §22.10 (v0.307, line 1861) post-#4868, `PBox3141`'s primary-ctor
// parameters are not auto-promoted. The local twin follows the canonical
// §22.10 idiom (docs/Bezel_Language_Spec.md:2042-2057): camelCase params
// `v`/`w`, PascalCase properties `V`/`W`, body reads in `Sum()` resolve to
// the captured fields (per §22.10's body-vs-init rule, line 1859). External
// `d.V`/`d.W` reads in Main remain valid.
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           local_primary_ctor_base_positional_fields_control) {
    BEZEL_EXPECT_EQ(compileLocalAndRun3141(
        "module LocF3_3141g; "
        "public class PBox3141(v: int, w: int) "
        "  public readonly property V: int := v; "
        "  public readonly property W: int := w; "
        "  public method Sum(): int return v + w; end Sum; "
        "end PBox3141; "
        "public class D3141(): PBox3141(5, 6) "
        "  public method Twice(): int return Sum() * 2; end Twice; "
        "end D3141; "
        "public class C "
        "  public static method Main(): int "
        "    var d := new D3141(); "
        "    if d.V <> 5 then return 91; end; "
        "    if d.W <> 6 then return 92; end; "
        "    return d.Twice() - 11; "
        "  end Main; "
        "end C;"), 11);
}

// ===========================================================================
// ★ T7 — the §9.7 null-source EXEMPTION, which #3145 (family F10) had just
// turned into a false reject. F10 made `isBorrowBearingRecord` answer correctly
// for an imported record; `isNullSourceRecordExpr` — the recognizer for the
// escape §9.7 itself names ("initialize with a value whose borrow-carrying
// components are all `null`") — still read the field list through the dead AST
// cast and so answered "not null-source" for every import. The two together
// rejected a program the local twin (T7b) compiles and runs. Returns tag = 7.
// // per §9.7 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_borrow_record_null_source_field_init_is_accepted) {
    BEZEL_EXPECT_EQ(compileImportAndRun3141(
        kLibNull,
        "module AppF3_3141h; import DepF3N_3141; "
        "public class Keeper3141 "
        "  public h: Holder3141 := new Holder3141(null, 7); "
        "  public method Tag(): int return h.tag; end Tag; "
        "end Keeper3141; "
        "public class C "
        "  public static method Main(): int "
        "    using var k := new Keeper3141(); "
        "    return k.Tag(); "
        "  end Main; "
        "end C;"), 7);
}

// ===========================================================================
// T7b — the local twin of T7. // per §9.7
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           local_borrow_record_null_source_field_init_control) {
    BEZEL_EXPECT_EQ(compileLocalAndRun3141(
        "module LocF3_3141h; "
        "public class Res3141 "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Res3141; "
        "public record Holder3141(r: Res3141?, tag: int); "
        "public class Keeper3141 "
        "  public h: Holder3141 := new Holder3141(null, 7); "
        "  public method Tag(): int return h.tag; end Tag; "
        "end Keeper3141; "
        "public class C "
        "  public static method Main(): int "
        "    using var k := new Keeper3141(); "
        "    return k.Tag(); "
        "  end Main; "
        "end C;"), 7);
}

// ===========================================================================
// ★ T8 — §14.2 `where T: new()`. A record with a non-nullable `Object` field
// has no well-defined `default(R)`, so it does not satisfy the bound. Reaching
// the field list through the AST left the verdict UNDECIDED for every import,
// which the caller reads as "keep the solution" — a silent accept of an
// instantiation the byte-identical local record is rejected for (T8b).
// // per §14.2 / §17.11 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_with_reference_field_fails_new_constraint) {
    auto r = compileImport3141(
        kLib,
        "module AppF3_3141i; import DepF3_3141; "
        "public class Holder<T> where T: new() "
        "  public N: int := 0; "
        "end Holder; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new Holder<RefRec3141>(); "
        "    return g.N; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(has(r.error,
                          "type argument `RefRec3141` does not satisfy the "
                          "constraint `where T: new()`"));
    BEZEL_EXPECT_TRUE(has(r.error, "(§14.2)"));
}

// ===========================================================================
// T8b — the local twin of T8, and T8c below is its complement: an IMPORTED
// primary-constructor CLASS is not parameterless either, so it fails the same
// bound. Both arms of `argSatisfiesNew`/`groundSatisfiesNew` are in this
// family. // per §14.2
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           local_record_with_reference_field_fails_new_constraint_control) {
    bool ok = true;
    std::string err = compileLocal3141(
        "module LocF3_3141i; "
        "public record RefRec3141(R: Object, N: int); "
        "public class Holder<T> where T: new() "
        "  public N: int := 0; "
        "end Holder; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new Holder<RefRec3141>(); "
        "    return g.N; "
        "  end Main; "
        "end C;", ok);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(has(err,
                          "type argument `RefRec3141` does not satisfy the "
                          "constraint `where T: new()`"));
}

// ===========================================================================
// T8c — the CLASS arm: an imported class with a parameterful primary
// constructor has no parameterless constructor, so `where T: new()` is
// unsatisfied. // per §14.2 / §22.10
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_primary_ctor_class_fails_new_constraint) {
    auto r = compileImport3141(
        kLib,
        "module AppF3_3141j; import DepF3_3141; "
        "public class Holder<T> where T: new() "
        "  public N: int := 0; "
        "end Holder; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new Holder<PBox3141>(); "
        "    return g.N; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(has(r.error,
                          "type argument `PBox3141` does not satisfy the "
                          "constraint `where T: new()`"));
}

// ===========================================================================
// ★ T10 — §14.1 / #428 phase B: an imported GENERIC record. Both
// instantiations must still construct and round-trip their values. This is the
// case a symbol-side check can most easily get WRONG in the over-rejecting
// direction — the field's declared type is the bare parameter `T`, so checking
// an argument against it unsubstituted would reject every instantiation.
// Returns V + N for the int arm and the string arm's length + N.
// // per §14.1 / §9.1 / §5.1
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_generic_record_instantiations_still_construct) {
    BEZEL_EXPECT_EQ(compileImportAndRun3141(
        kLib,
        "module AppF3_3141l; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var b := new GBox3141<int>(5, 7); "
        "    if b.V <> 5 then return 91; end; "
        "    if b.N <> 7 then return 92; end; "
        "    var s := new GBox3141<string>(\"hi\", 8); "
        "    if s.V <> \"hi\" then return 93; end; "
        "    return s.N + 4; "
        "  end Main; "
        "end C;"), 12);
}

// ===========================================================================
// T10b — the reject side of T10, and the reason the substitution is there at
// all: `new GBox3141<int>("x", 7)` binds a `string` to a field the
// instantiation types `int`. Without substituting `T` through the target's
// generic arguments this deferred, which is exactly the hole #2203 closed for
// the local path ("skipping generic records used to admit
// `new Wrap<string>(\"x\")` for `record Wrap<W>(tag: int)`"). // per §14.1 / §5.1
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_generic_record_wrong_typed_arg_is_rejected) {
    auto r = compileImport3141(
        kLib,
        "module AppF3_3141m; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var b := new GBox3141<int>(\"x\", 7); "
        "    return b.N; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(has(r.error, "the argument `V` to `new GBox3141`"));
    BEZEL_EXPECT_TRUE(has(r.error, "(§5.1)"));
}

// ===========================================================================
// T10c — the local twin of T10b. // per §14.1 / §5.1
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           local_generic_record_wrong_typed_arg_control) {
    bool ok = true;
    std::string err = compileLocal3141(
        "module LocF3_3141m; "
        "public record GBox3141<T>(V: T, N: int); "
        "public class C "
        "  public static method Main(): int "
        "    var b := new GBox3141<int>(\"x\", 7); "
        "    return b.N; "
        "  end Main; "
        "end C;", ok);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(has(err, "the argument `V` to `new GBox3141`"));
}

// ===========================================================================
// ★ T11 — THE ONE-TOKEN VARIATION. `new Named3141(ref n, 2)` with `n: int` is
// T1 plus one keyword, and the first version of this fix let it through: the
// symbol-side check opened with a blanket "any argument carries a modifier ->
// defer", borrowed from checkImportedNewExprArgs (where it is right, because an
// imported CONSTRUCTOR may really declare `ref`/`out` parameters). A §22.10
// primary-constructor parameter list admits only plain and `owned`, so no
// modifier can change what field i is — and the program compiled and SIGSEGV'd,
// i.e. THE HEADLINE DEFECT SURVIVED ONE ADDED TOKEN. §22.3/§14.5 give the rule:
// a `ref` argument aliases the caller's location, so the type must match
// exactly. // per §22.3 / §14.5 / §9.1 / §2.8.3 rule 4, v0.254
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_ref_arg_of_wrong_type_is_rejected) {
    auto r = compileImport3141(
        kLib,
        "module AppF3_3141n; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var n: int := 5; "
        "    var p := new Named3141(ref n, 2); "
        "    return p.N; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(has(r.error,
                          "the `ref` argument for parameter `Tag` to "
                          "`new Named3141`"));
    BEZEL_EXPECT_TRUE(has(r.error, "(§14.5)"));
}

// ===========================================================================
// T11b — the local twin of T11, same wording. // per §22.3 / §14.5
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           local_record_ref_arg_of_wrong_type_control) {
    bool ok = true;
    std::string err = compileLocal3141(
        "module LocF3_3141n; "
        "public record Named3141(Tag: string, N: int); "
        "public class C "
        "  public static method Main(): int "
        "    var n: int := 5; "
        "    var p := new Named3141(ref n, 2); "
        "    return p.N; "
        "  end Main; "
        "end C;", ok);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(has(err,
                          "the `ref` argument for parameter `Tag` to "
                          "`new Named3141`"));
    BEZEL_EXPECT_TRUE(has(err, "(§14.5)"));
}

// ===========================================================================
// T11c — the other direction: a `string` local passed as `ref` to an `int`
// field. Pre-fix this compiled and ran (exit 146 here — the value depends on
// the pointer, which is the point). // per §22.3 / §14.5
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_ref_string_into_int_field_is_rejected) {
    auto r = compileImport3141(
        kLib,
        "module AppF3_3141o; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var s: string := \"abcd\"; "
        "    var p := new Pt3141(ref s, 2); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(has(r.error,
                          "the `ref` argument for parameter `X` to "
                          "`new Pt3141`"));
}

// ===========================================================================
// T11d — arity is modifier-INDEPENDENT: `new Pt3141(ref a)` is one argument for
// two fields whatever the modifier says, so §22.5 must fire before any §5.1 /
// §14.5 reasoning. The blanket deferral skipped this too. // per §22.5
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_ref_arg_wrong_arity_is_rejected) {
    auto r = compileImport3141(
        kLib,
        "module AppF3_3141p; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var a: int := 3; "
        "    var p := new Pt3141(ref a); "
        "    return p.X; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(
        has(r.error, "`new Pt3141`: required parameter `Y` is not supplied"));
}

// ===========================================================================
// ★ T11e — THE OVER-REJECTION CONTROL for T11. A `ref` argument whose type
// MATCHES the field's is what the local path ACCEPTS (measured: `new
// Pt3141(ref a, ref b)` with two `int` locals compiles and runs), so the
// imported path must accept it too. Without this, "reject every modified
// argument" would pass T11 while being a new false reject — and diverging from
// the local answer is the exact failure this family is about. Returns 3 + 4.
// // per §22.3 / §14.5
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_record_ref_arg_of_matching_type_is_accepted) {
    BEZEL_EXPECT_EQ(compileImportAndRun3141(
        kLib,
        "module AppF3_3141q; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var a: int := 3; "
        "    var b: int := 4; "
        "    var p := new Pt3141(ref a, ref b); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end C;"), 7);
}

// ===========================================================================
// T11f — the local twin of T11e: it ran before the fix and must still.
// // per §22.3 / §14.5
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           local_record_ref_arg_of_matching_type_control) {
    BEZEL_EXPECT_EQ(compileLocalAndRun3141(
        "module LocF3_3141q; "
        "public record Pt3141(X: int, Y: int); "
        "public class C "
        "  public static method Main(): int "
        "    var a: int := 3; "
        "    var b: int := 4; "
        "    var p := new Pt3141(ref a, ref b); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end C;"), 7);
}

// ===========================================================================
// T9 — the half #3141 DECLINED, now the half #3162 CONSTRUCTS.
//
// #3141 left `new ImportedPBox(3, 4)` rejected — with the right reason, but
// rejected: the dependency exports only `bz$<Type>$init(ptr)`, so the primary
// constructor's parameters appeared to have nowhere to ride across. This row
// pinned that decline so it could not regress silently.
//
// fix #3162 (§22.10 / §22.8 step 0, v0.254) makes the construction work, so
// the row FLIPS from a diagnostic assertion to a VALUE assertion — which is
// what the decline's own comment said should happen ("Once construction works,
// the sema gate ... should be replaced by an ordinary §22.5/§5.1 argument
// check"). The gap was an ORDERING one, not a channel one: §22.8 makes storing
// the primary parameters the CALLER's step 0, and the exported `init` is steps
// 1+2 against the object it is handed, so `lowerNew`'s imported arm stores the
// arguments and then calls it. Nothing new crosses the wire.
//
// `Sum()` = 3 + 4 = 7; a calloc-zero object — the outcome the decline existed
// to prevent — reads 0. The full surface (field initializers reading a primary
// parameter, `owned` capture, base-clause arguments, the §22.5/§5.1 rejects
// this now routes through, and the over-rejection controls) lives in
// tests/module/xmod_imported_primary_ctor_3162_test.cpp.
// // per §22.10 / §22.8 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           imported_primary_ctor_class_constructs_3162) {
    BEZEL_EXPECT_EQ(compileImportAndRun3141(
        kLib,
        "module AppF3_3141k; import DepF3_3141; "
        "public class C "
        "  public static method Main(): int "
        "    var b := new PBox3141(3, 4); "
        "    return b.Sum(); "
        "  end Main; "
        "end C;"), 7);
}

// ===========================================================================
// T9b — the local twin of T9, byte-identical apart from the module boundary.
// It passed before #3162 and must still. // per §22.10 / §22.8
// ===========================================================================
// T9b — the local twin of T9, byte-identical apart from the module boundary.
// It passed before #3162 and must still. // per §22.10 / §22.8
//
// Per spec §22.10 (v0.307, line 1861) post-#4868, `PBox3141` declares
// `public readonly property` accessors for its primary-ctor params. The
// canonical idiom (docs/Bezel_Language_Spec.md:2042-2057) uses camelCase
// params and PascalCase properties; this control observes the local build
// stays working via `b.Sum()` (which reads the in-class captured fields, line
// 1859), so the test pins that the byte-identical local behavior is unchanged
// — the property declarations are bookkeeping-only here.
BEZEL_TEST(xmod_imported_positional_new_args_3141,
           local_primary_ctor_class_constructs_control) {
    BEZEL_EXPECT_EQ(compileLocalAndRun3141(
        "module LocF3_3141k; "
        "public class PBox3141(v: int, w: int) "
        "  public readonly property V: int := v; "
        "  public readonly property W: int := w; "
        "  public method Sum(): int return v + w; end Sum; "
        "end PBox3141; "
        "public class C "
        "  public static method Main(): int "
        "    var b := new PBox3141(3, 4); "
        "    return b.Sum(); "
        "  end Main; "
        "end C;"), 7);
}

}  // namespace bezel::module
