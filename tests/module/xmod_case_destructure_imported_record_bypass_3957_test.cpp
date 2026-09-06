// fix #3957 — §19.3 / §9.1 / §9.2 / §2.8.3 rule 4 (v0.294): an IMPORTED
// (cross-module) record scrutinee bypassed EVERY §19.3 case-destructuring
// sema check.
//
// ── ROOT CAUSE ───────────────────────────────────────────────────────────
//
// `CaseExpr`'s and `CaseStmt`'s record-scrutinee `DestructuringPattern`
// handlers located the record `TypeSymbol` named by the pattern's head with a
// LOCAL-ONLY symbol-table search (`ctx_.symbols()` for CaseExpr;
// `ctx_.module()->children` — even narrower — for CaseStmt), with no
// fallback to `ctx_.importedSymbols()`. When the scrutinee's record was
// imported, `recTy` stayed null, `fields` stayed empty, and every downstream
// §19.3 check that depends on it was silently skipped:
//
//   * the arity check ("the binding count must match", §19.3 / Appendix C),
//   * the #2178 generic-erasure rejection ("a box of ANY instantiation would
//     match…"),
//   * the #3692/#3898 nullable-ordering-pattern gate ("a relational or range
//     pattern requires a non-nullable scrutinee…").
//
// A SECOND, independent gap compounded the ordering-pattern check even once
// `recTy` resolves: the field-type read at each destructured slot used the
// bare `fields[i]->posParam->type`, which fix #2516 documents as
// DELIBERATELY NULL for an imported record's positional field (the real type
// lives in `ctx_.importedMemberType`, reached only through the shared
// `storageComponentType` accessor, sema/Symbol.hpp). So even after `recTy`
// resolves, the ordering gate saw a null field type and never fired — this
// is why the (c) NESTED case below needed a second fix
// (`bindNestedDestructuringNames`) beyond the (a)/(b)/(d) top-level ones.
//
// The fix mirrors codegen's own already-import-aware lookup
// (`LlvmBackend_Exprs2.cpp`'s `patternMatches`, fix #2844) and this file's
// own `bindLambdaDestructure` (also already import-aware): fall back to
// `ctx_.importedSymbols()` after the local search misses, and read every
// destructured field's type through `storageComponentType` instead of the
// bare `posParam->type`. The SAME local-only-lookup defect existed for the
// CLASS-detection loop right beside the record search (an imported class
// used as a destructuring head reached codegen unrejected and crashed with
// malformed IR instead of getting the §19.3 "not valid on a class type"
// diagnostic); that loop gets the identical fallback in the same commit.
//
// The choice-scrutinee sibling (`findChoiceCaseOnScrutinee`) was NOT
// affected — it starts from the scrutinee's own resolved
// `TypeRef::declaration`, which import machinery already populates
// correctly — so it is exercised here only as an anti-regression control.
//
// ── MEASURED, on the fix's parent (pre-fix) ─────────────────────────────
//
//   family              imported                    local twin
//   arity               codegen ICE: "does not have  clean §19.3 reject
//                       terminator! label %case.end"
//   #2178 erasure       compiles, runs to CORRECT     clean §19.3/§12.7
//                       value (silent over-admission) reject (#2178)
//   nullable ordering   compiles, runs to CORRECT      clean §19.3/§16.6/
//                       value (silent over-admission)  §7.11 reject (#3692)
//   class-as-head       codegen ICE (same shape as     clean §19.3 reject
//                       arity)
//   nested (choice      compiles, runs to CORRECT      clean §19.3/§16.6/
//   payload holding an  value (silent over-admission)  §7.11 reject
//   imported record)
//
// No memory-unsafety was found in any pre-fix case: codegen's own
// import-aware lookup (paired with fix #3899) computes the correct runtime
// value whenever the program links at all; the arity and class-as-head
// families instead hit a compile-time IR-verification failure (a clean
// `exit 1`, not a crash) rather than the intended diagnostic.
//
// ── ORACLE CHOICE ────────────────────────────────────────────────────────
//
// AOT execution via `Driver::compileObject` (library) +
// `bezel::test::compileFileForTest` (importer) is the oracle for every
// cross-module case — the cross-module shape is invisible to the
// single-module `runAot` harness. The single-module local-twin and
// anti-regression controls use `runAot` directly.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

const std::filesystem::path& wtDir3957() { return wtScratchDir("wt-3957"); }

struct TempFile3957 {
    std::string path;
    explicit TempFile3957(std::string p) : path(std::move(p)) {}
    ~TempFile3957() { std::remove(path.c_str()); }
};

void writeSource3957(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string captureStderr3957(const std::function<void()>& body) {
    std::string errPath =
        (wtDir3957() / ("err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    body();
    std::fflush(stderr);
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return err;
}

struct XmodResult3957 {
    bool libOk = false;
    bool appOk = false;
    std::string err;
    std::string exePath;
};

// Compiles `libSrc` to an object, then compiles `appSrc` against it as a
// dependency, capturing stderr from the APP compile. Used by both the
// rejection tests (appOk expected false, err checked) and the positive
// controls (appOk expected true, exePath then run).
XmodResult3957 compileImport3957(const std::string& libSrc,
                                 const std::string& appSrc) {
    auto dir = wtDir3957();
    std::string stem = unique_stem("bz3957_");
    XmodResult3957 out;
    TempFile3957 libSrcFile((dir / (stem + "_lib.bzl")).string());
    TempFile3957 libObjFile((dir / (stem + "_lib.o")).string());
    writeSource3957(libSrcFile.path, libSrc);
    {
        bezel::Driver driver;
        out.libOk = driver.compileObject(libSrcFile.path, libObjFile.path).success
            && std::ifstream(libObjFile.path).good();
    }
    if (!out.libOk) return out;
    TempFile3957 appSrcFile((dir / (stem + "_app.bzl")).string());
    std::string exePath = (dir / stem).string();
    writeSource3957(appSrcFile.path, appSrc);
    out.err = captureStderr3957([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = libObjFile.path;
        d.objectPath = libObjFile.path;
        deps.push_back(d);
        out.appOk =
            bezel::test::compileFileForTest(driver, appSrcFile.path, exePath, deps)
                .success;
    });
    out.exePath = exePath;
    return out;
}

int runCapture3957(const std::string& exe) {
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

bool contains3957(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── LIBRARIES ────────────────────────────────────────────────────────────

const char* kLibPlainRecord3957 =
    "module Lib3957Plain; public record R3957(X: int?, Y: int);";

const char* kLibGenericRecord3957 =
    "module Lib3957Generic; public record Box3957<T>(X: T, Y: int);";

const char* kLibClass3957 =
    "module Lib3957Class; "
    "public class K3957 "
    "  public V: int; "
    "  public constructor(v: int) this.V := v; end constructor; "
    "end K3957;";

const char* kLibNestedInner3957 =
    "module Lib3957Nested; public record Inner3957(X: int?, Y: int);";

// ── (a) ARITY — imported record, expression-position `case` ─────────────
//
// `R3957(X: int?, Y: int)` has 2 positional fields; the pattern binds 3.
// Pre-fix: recTy never resolves (imported), so sema's arity check never
// runs and the program reaches codegen, which crashes on malformed IR
// ("Basic Block ... does not have terminator! label %case.end") instead of
// this clean §19.3 diagnostic. Post-fix: rejected with the arity message.
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           arity_mismatch_imported_record_expr_case_spec_19_3) {
    const std::string app =
        "module App3957Arity; "
        "import Lib3957Plain; "
        "class C public static method Main(): int "
        "  var r := new R3957(99, 1); "
        "  return (case r of R3957(x, y, z) => 0, _ => 3); "
        "end Main; end C;";
    auto r = compileImport3957(kLibPlainRecord3957, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(
        contains3957(r.err, "the binding count must match (§19.3)"));
}

// ── (b) #2178 GENERIC ERASURE — imported generic record, Object scrutinee ─
//
// `Box3957<int>` boxed through `Object`; the destructuring pattern names no
// type arguments and the scrutinee's static type (`Object`) cannot
// determine the instantiation. Pre-fix: compiles cleanly and runs to the
// CORRECT value (9) — codegen's own import-aware lookup gets it right, but
// sema never rejects the erased pattern §19.3/§12.7/#2178 requires. Post-
// fix: rejected.
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           generic_erasure_imported_record_2178_spec_19_3_12_7) {
    const std::string app =
        "module App3957Erasure; "
        "import Lib3957Generic; "
        "class C public static method Main(): int "
        "  var b := new Box3957<int>(9, 1); "
        "  var o: Object := b; "
        "  return (case o of Box3957(x, y) => x, _ => 3); "
        "end Main; end C;";
    auto r = compileImport3957(kLibGenericRecord3957, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3957(
        r.err,
        "is generic but names no type arguments"));
}

// Second instantiation of (b) — a different type argument (`string`) proves
// the erasure rejection is not accidentally keyed on `int` specifically.
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           generic_erasure_imported_record_string_instantiation_spec_19_3) {
    const std::string app =
        "module App3957ErasureStr; "
        "import Lib3957Generic; "
        "class C public static method Main(): int "
        "  var b := new Box3957<string>(\"hi\", 1); "
        "  var o: Object := b; "
        "  return (case o of Box3957(x, y) => y, _ => 3); "
        "end Main; end C;";
    auto r = compileImport3957(kLibGenericRecord3957, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3957(
        r.err,
        "is generic but names no type arguments"));
}

// ── (c) NULLABLE ORDERING — imported record, expression-position `case` ──
//
// This is issue #3957's own headline repro. `R3957.X: int?`; the pattern
// puts a range sub-pattern (`1..5`) on that nullable slot. Pre-fix:
// compiles cleanly and RUNS TO THE CORRECT VALUE (3 — 99 is out of range,
// falls to `_`), silently admitting what §19.3/§16.6/§7.11 (#3692) requires
// rejecting. Post-fix: rejected with the exact ordering-pattern diagnostic.
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           nullable_ordering_imported_record_expr_case_spec_19_3_16_6_7_11) {
    const std::string app =
        "module App3957Ordering; "
        "import Lib3957Plain; "
        "class C public static method Main(): int "
        "  var r := new R3957(99, 1); "
        "  return (case r of R3957(1..5, 1) => 7, _ => 3); "
        "end Main; end C;";
    auto r = compileImport3957(kLibPlainRecord3957, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3957(
        r.err,
        "a range pattern requires a non-nullable scrutinee"));
}

// ── (d) STATEMENT-FORM `case` — same nullable-ordering family ────────────
//
// The issue's CaseStmt handler (ExprTypecheck.cpp) is a SEPARATE code path
// from CaseExpr and was even narrower pre-fix (module-children only, not
// even `ctx_.symbols()`). Same library, statement-position `case`.
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           nullable_ordering_imported_record_stmt_case_spec_19_3) {
    const std::string app =
        "module App3957OrderingStmt; "
        "import Lib3957Plain; "
        "class C public static method Main(): int "
        "  var r := new R3957(99, 1); "
        "  case r of "
        "    R3957(1..5, 1) => do return 7; end, "
        "    _ => do return 3; end "
        "  end; "
        "end Main; end C;";
    auto r = compileImport3957(kLibPlainRecord3957, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3957(
        r.err,
        "a range pattern requires a non-nullable scrutinee"));
}

// ── (e) IMPORTED CLASS AS DESTRUCTURING HEAD ─────────────────────────────
//
// `K3957` is a class, not a record — §19.3 says classes have no positional-
// destructuring surface at all. Pre-fix the class-detection loop was ALSO
// local-only, so an imported class reached codegen unrejected and crashed
// with the same "no terminator" malformed-IR failure the arity case hits.
// Post-fix: the clean "not valid on a class type" diagnostic.
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           imported_class_as_destructuring_head_spec_19_3) {
    const std::string app =
        "module App3957Class; "
        "import Lib3957Class; "
        "class C public static method Main(): int "
        "  var k := new K3957(5); "
        "  return (case k of K3957(v) => v, _ => 3); "
        "end Main; end C;";
    auto r = compileImport3957(kLibClass3957, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(
        contains3957(r.err, "is not valid on a class type"));
}

// ── (f) NESTED — an imported record nested inside a LOCAL choice payload ──
//
// `Outer.Wrap(inner: Inner3957)` is a LOCAL choice whose payload type
// `Inner3957` is IMPORTED. The nested pattern `Wrap(Inner3957(1..5, 1))`
// exercises `bindNestedDestructuringNames`'s `fieldsOfRecord` helper (fix
// #2844 already made ITS recTy lookup import-aware) — but that helper's
// field-TYPE read still used the bare `posParam->type`, so even with `recTy`
// resolved the ordering gate saw a null field type and never fired. This is
// the SECOND fix in this issue (storageComponentType, not just the
// recTy lookup). Pre-fix: compiles, runs to the correct value (3). Post-
// fix: rejected.
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           nested_imported_record_in_local_choice_payload_spec_19_3) {
    const std::string app =
        "module App3957Nested; "
        "import Lib3957Nested; "
        "choice Outer3957 Wrap3957(inner: Inner3957), None3957; end Outer3957; "
        "class C public static method Main(): int "
        "  var o := Outer3957.Wrap3957(new Inner3957(99, 1)); "
        "  return (case o of Wrap3957(Inner3957(1..5, 1)) => 7, _ => 3); "
        "end Main; end C;";
    auto r = compileImport3957(kLibNestedInner3957, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3957(
        r.err,
        "a range pattern requires a non-nullable scrutinee"));
}

// ── (g) ANTI-REGRESSION CONTROL — imported CHOICE scrutinee still rejects ─
//
// Issue #3957 measured that the choice-scrutinee sibling
// (`findChoiceCaseOnScrutinee`) was NOT broken (it starts from the
// scrutinee's own resolved `TypeRef::declaration`, which import machinery
// already populates). Pin that this fix did not regress it.
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           imported_choice_ordering_still_rejects_control_spec_19_3) {
    const char* lib =
        "module Lib3957Choice; "
        "public choice Wrap3957c<T> Val3957c(X: T, Y: int), None3957c; "
        "end Wrap3957c;";
    const std::string app =
        "module App3957Choice; "
        "import Lib3957Choice; "
        "class C public static method Main(): int "
        "  var w := Wrap3957c<int?>.Val3957c(99, 1); "
        "  return (case w of Val3957c(1..5, 1) => 7, _ => 3); "
        "end Main; end C;";
    auto r = compileImport3957(lib, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.appOk);
    BEZEL_EXPECT_TRUE(contains3957(
        r.err,
        "a range pattern requires a non-nullable scrutinee"));
}

// ── (h) POSITIVE CONTROL — a CORRECT cross-module destructuring must still
//        compile and run to the right value ───────────────────────────────
//
// Same imported generic record, statically typed (not erased through
// Object) so #2178 does not apply, correct arity, and no ordering
// sub-pattern on the nullable-capable slot — this program is valid under
// every §19.3 check this fix restores and MUST keep compiling. Guards
// against the fix over-rejecting cross-module record destructuring
// entirely.
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           positive_control_valid_imported_generic_record_destructure) {
    const std::string app =
        "module App3957Positive; "
        "import Lib3957Generic; "
        "class C public static method Main(): int "
        "  var b := new Box3957<int>(9, 1); "
        "  return (case b of Box3957(x, y) => x + y, _ => 0); "
        "end Main; end C;";
    auto r = compileImport3957(kLibGenericRecord3957, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3957(r.exePath), 10);
}

// ── (i) POSITIVE CONTROL — plain (non-generic) imported record, discard
//        slot, must still compile and run to the right value ─────────────
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           positive_control_valid_imported_plain_record_destructure) {
    const std::string app =
        "module App3957Positive2; "
        "import Lib3957Plain; "
        "class C public static method Main(): int "
        "  var r := new R3957(4, 5); "
        "  return (case r of R3957(x, _) when x <> null => x!, _ => 0); "
        "end Main; end C;";
    auto r = compileImport3957(kLibPlainRecord3957, app);
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.appOk);
    if (!r.appOk) return;
    BEZEL_EXPECT_EQ(runCapture3957(r.exePath), 4);
}

// ── (j) LOCAL TWIN — anti-regression: the identical program with the
//        record declared LOCALLY must keep rejecting exactly as before ────
BEZEL_TEST(xmod_case_destructure_imported_record_bypass_3957,
           local_twin_still_rejects_ordering_control_spec_19_3) {
    const char* prog =
        "module LocalTwin3957; "
        "record R3957Local(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R3957Local(99, 1); "
        "  return (case r of R3957Local(1..5, 1) => 7, _ => 3); "
        "end Main; end C;";
    auto r = bezel::test::runAot(prog);
    // A rejected compile: runAot's `ran` is false and it packs the driver's
    // captured stderr into one Error diagnostic (see aot_support.hpp's
    // runAot header comment) — check the actual TEXT, not just `hasErrors()`.
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawOrderingDiag = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            contains3957(d.message,
                        "a range pattern requires a non-nullable scrutinee"))
            sawOrderingDiag = true;
    }
    BEZEL_EXPECT_TRUE(sawOrderingDiag);
}

}  // namespace bezel::module
