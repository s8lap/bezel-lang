// Regression tests for #4592 — §22.16 / §2.8.2 (v0.306): a class invariant
// declared on an IMPORTED class must be evaluated at §22.16's instrumentation
// points on the importing side of a module boundary.
//
// THE DEFECT. `Emitter::emitInvariantCheck` (src/codegen/LlvmBackend_Setup.cpp)
// builds the base-first chain of predicates to evaluate by walking the AST:
//
//     for (const sema::TypeSymbol* c = classType; c; c = baseClassOf(c)) {
//         const auto* td = dynamic_cast<const ast::TypeDecl*>(c->decl);
//         if (!td) continue;          // imported: invariant records, no AST
//         ...
//
// `TypeSymbol::decl` is never assigned for an imported type, so that `continue`
// fired for EVERY imported class and dropped its invariants from the
// conjunction outright. Not "checked somewhere else": §22.16 puts the
// construction-exit check at "the normal completion of the most-derived
// constructor", which codegen wires at the `new` SITE — and the site is in the
// importing module, so nothing the exporter compiled stood in for it.
//
// WHY THE SYMBOL SIDE COULD NOT JUST BE READ (unlike the sibling #3152-family
// gaps). `TypeSymbol::invariantContracts` DOES cross — but a §2.8.2 contract
// record carries `canonicalText`, which that section defines with every name
// "recorded as resolved at the declaration (module-qualified)". MEASURED: the
// `invariant(v > 0);` in `kInvLib4592` below arrives as the string
// `InvLib4592.InvBase4592.v > 0`, which is an identity artifact, not an
// expression that re-parses into something evaluable against `this`. The fix
// therefore adds the second, identity-free channel §2.8.2 itself anticipates
// ("An implementation may additionally carry each clause's display text
// (original names)"): `MetaTypeDecl::ownInvariantText` carries the clause
// source verbatim, `ModuleLoader::parseAndAttachInvariants` re-parses it in a
// synthetic re-declaration of the class and remaps the result onto the imported
// member symbols, and codegen reads the published expressions.
//
// MEASURED ACROSS THE FIX BOUNDARY (the CLI repro, both halves built
// `--checked-contracts`, one module importing the other):
//     row 1 derived-from-imported-base   pre-fix 42   post-fix 99
//     row 2 directly-imported class      pre-fix 42   post-fix 99
//     row 3 public method exit           pre-fix 42   post-fix 77
//     row 4 conforming program            7 on both sides
//     row 5 same-module control          99 on both sides  <- the boundary IS
//                                                              the difference
//     row 6 release (unchecked) mode     42 on both sides
// A module whose classes declare no invariant serializes BYTE-IDENTICALLY
// across the fix (SHA-256 of the `.bezel.meta` section of a three-type probe
// module, measured on both compilers), which is what keeps §2.8.3 rule 3's
// staleness hash from moving for a change that wrote nothing.
//
// THE TEST. Every violating row crosses a REAL `--import` boundary — an
// in-module test cannot pin this bug, because the local shape was never broken
// (row 5 is exactly that control). The AOT harness (`runAot`/`runAotChecked`)
// compiles ONE module and cannot express a dependency, so these rows hand-roll
// `Driver::compileObject` for the library plus `bezel::test::compileFileForTest`
// for the importer — the established two-module shape
// (tests/module/xmod_checked_mode_forwarding_4488_test.cpp,
// tests/module/cross_module_import_test.cpp) — and run the product through
// `bezel::test::runProgramForTest` rather than `std::system`, so the sweep's own
// run options apply and LeakSanitizer's exit code does not replace the
// program's (#4532). No JIT oracle is used.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile4592 {
    std::string path;
    explicit TempFile4592(std::string p) : path(std::move(p)) {}
    ~TempFile4592() { std::remove(path.c_str()); }
};

void writeSource4592(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build `libSrc` as a `-c` library OBJECT, then build `mainSrc` as an
// executable importing it, run it, and return the program's exit code. An
// EMPTY `libSrc` skips the library half entirely and compiles `mainSrc` on its
// own — that is how row 5's same-module control runs the byte-identical
// program with no boundary at all.
//
// Distinct negative sentinels, so a broken scaffold can NEVER read as a pass:
// every row below asserts an exact non-negative value.
//   -2  the library half did not build (or produced no object)
//   -3  the importer half did not build/link
//   -4  the program could not be run to a normal exit
int buildImportAndRun4592(const std::string& libSrc, const std::string& mainSrc,
                          const codegen::CodegenOptions& opts, const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4592 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4592 libObjF((dir / (id + "_lib.o")).string());
    TempFile4592 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4592 appExeF((dir / id).string());
    writeSource4592(appSrcF.path, mainSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    if (!libSrc.empty()) {
        writeSource4592(libSrcF.path, libSrc);
        // The library is built in the SAME mode as the app: #4488 established
        // that both emitted modes must cross the boundary, and a row here that
        // built the library unchecked would be measuring that fix, not this one.
        codegen::CodegenOptions libOpts = opts;
        libOpts.isMainModule = false;
        if (!driver.compileObject(libSrcF.path, libObjF.path, {}, libOpts)
                 .success)
            return -2;
        if (!std::ifstream(libObjF.path).good()) return -2;
        bezel::Dependency d;
        d.metadataPath = libObjF.path;
        d.objectPath = libObjF.path;
        deps.push_back(d);
    }

    // compileFileForTest, not compileFile, so `gate.sh --full`'s
    // BEZEL_AOT_FORCE_SANITIZE sweep reaches the importer half (#3653).
    if (!bezel::test::compileFileForTest(driver, appSrcF.path, appExeF.path,
                                          deps, opts)
             .success)
        return -3;

    int status = bezel::test::runProgramForTest(appExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -4;
}

codegen::CodegenOptions checkedContractsOpts4592() {
    codegen::CodegenOptions o;
    o.checkedContracts = true;  // exactly what `bezelc --checked-contracts` sets
    return o;
}

// ── the library ──────────────────────────────────────────────────────────
// `InvBase4592` declares the §22.16 invariant. Its constructor deliberately
// stores whatever it is given, so the violating value reaches construction
// exit rather than being rejected inside the exporter.
const char* kInvLib4592 =
    "module InvLib4592; "
    "public class InvBase4592 "
    "  public v: int := 1; "
    "  invariant(v > 0); "
    "  public constructor(x: int) "
    "    v := x; "
    "  end constructor; "
    "end InvBase4592; ";

// ── row 1: a LOCAL class deriving from the imported base ─────────────────
// §22.16 "The class invariant of C is the conjunction of every invariant member
// of C and of every base class, evaluated base-first". `DerB4592` satisfies its
// own clause and violates the inherited one, so only a chain that reaches the
// IMPORTED link faults.
const char* kInvAppDerived4592 =
    "module InvApp4592; "
    "import InvLib4592; "
    "class DerB4592 : InvBase4592 "
    "  public w: int := 0; "
    "  invariant(w >= 0); "
    "  public constructor(x: int, y: int) "
    "    base(x); "
    "    w := y; "
    "  end constructor; "
    "end DerB4592; "
    "class MainB4592 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var d := new DerB4592(-1, 0); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainB4592; ";

// ── row 2: the imported class constructed DIRECTLY ───────────────────────
// No local subclass at all — `classType` at the `new` site IS the imported
// symbol, so the chain was empty rather than merely short.
const char* kInvAppDirect4592 =
    "module InvAppD4592; "
    "import InvLib4592; "
    "class MainD4592 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var b := new InvBase4592(-1); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainD4592; ";

// ── row 3: public method EXIT, a different instrumentation point ─────────
// §22.16 instruments "Entry and normal exit of every `public` instance method".
// `Break` is declared on the LOCAL subclass, so its exit check is emitted by
// the importing compilation over a chain whose only invariant is the imported
// base's.
const char* kInvAppMethodExit4592 =
    "module InvAppM4592; "
    "import InvLib4592; "
    "class DerM4592 : InvBase4592 "
    "  public constructor(x: int) "
    "    base(x); "
    "  end constructor; "
    "  public method Break(): int "
    "    v := -1; "
    "    return v; "
    "  end Break; "
    "end DerM4592; "
    "class MainM4592 "
    "  public static method Main(): int "
    "    using var d := new DerM4592(1); "
    "    var r: int := 0; "
    "    try "
    "      var x := d.Break(); "
    "      r := 42; "
    "    catch ContractViolationException => r := 77; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainM4592; ";

// ── row 4: the conforming program over the same library ──────────────────
// Both clauses hold, so construction completes and the imported base's storage
// reads back through the re-parsed predicate's own field binding.
const char* kInvAppConforming4592 =
    "module InvAppOk4592; "
    "import InvLib4592; "
    "class DerOk4592 : InvBase4592 "
    "  public w: int := 0; "
    "  invariant(w >= 0); "
    "  public constructor(x: int, y: int) "
    "    base(x); "
    "    w := y; "
    "  end constructor; "
    "  public method Sum(): int "
    "    return v + w; "
    "  end Sum; "
    "end DerOk4592; "
    "class MainOk4592 "
    "  public static method Main(): int "
    "    using var d := new DerOk4592(5, 2); "
    "    return d.Sum(); "
    "  end Main; "
    "end MainOk4592; ";

// ── row 5: the SAME-MODULE control ───────────────────────────────────────
// Byte-for-byte the same declarations and the same violating construction as
// row 1, with the base declared locally instead of imported. This is the row
// that makes row 1 a statement about the BOUNDARY rather than about invariants
// in general: it answered 99 on the pre-fix compiler too.
const char* kInvLocalControl4592 =
    "module InvLocal4592; "
    "class LocBase4592 "
    "  public v: int := 1; "
    "  invariant(v > 0); "
    "  public constructor(x: int) "
    "    v := x; "
    "  end constructor; "
    "end LocBase4592; "
    "class LocDer4592 : LocBase4592 "
    "  public w: int := 0; "
    "  invariant(w >= 0); "
    "  public constructor(x: int, y: int) "
    "    base(x); "
    "    w := y; "
    "  end constructor; "
    "end LocDer4592; "
    "class MainL4592 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var d := new LocDer4592(-1, 0); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainL4592; ";

}  // namespace

// ===========================================================================
// Row 1 — THE BUG (§22.16 "Construction exit", v0.306). A local class derives
// from an imported base carrying `invariant(v > 0);` and is constructed with
// v = -1. §22.16: "Every fresh instance satisfies its full dynamic class
// invariant before it is observable; a violation raises
// ContractViolationException" — so the catch arm must run and the program must
// exit 99.
//
// Pre-fix this exited 42: `emitInvariantCheck`'s `if (!td) continue` dropped
// the imported link and the derived class's own (satisfied) clause was the
// whole conjunction. Measured pre-fix 42, post-fix 99.
// ===========================================================================
BEZEL_TEST(xmod_imported_base_invariant_4592,
           imported_base_invariant_fires_at_construction_exit_spec_22_16) {
    const int rc = buildImportAndRun4592(kInvLib4592, kInvAppDerived4592,
                                         checkedContractsOpts4592(),
                                         "wt-4592-derived");
    BEZEL_EXPECT_EQ(99, rc);
}

// ===========================================================================
// Row 2 — the same defect with no local subclass at all: `new InvBase4592(-1)`
// on the imported type itself. The chain was EMPTY here, not merely short, so
// this row distinguishes "the base link was skipped" from "the walk stopped
// early". Measured pre-fix 42, post-fix 99.
// ===========================================================================
BEZEL_TEST(xmod_imported_base_invariant_4592,
           directly_imported_class_invariant_fires_at_new_spec_22_16) {
    const int rc = buildImportAndRun4592(kInvLib4592, kInvAppDirect4592,
                                         checkedContractsOpts4592(),
                                         "wt-4592-direct");
    BEZEL_EXPECT_EQ(99, rc);
}

// ===========================================================================
// Row 3 — a DIFFERENT §22.16 instrumentation point over the same gap: "Entry
// and normal exit of every `public` instance method". A public method on the
// local subclass writes the imported base's field to -1 and returns; the exit
// check must fault. This is not a duplicate of row 1: construction exit is
// emitted at the `new` site while the method check is emitted in the method's
// own prologue/epilogue, and both read the same chain builder.
// Measured pre-fix 42, post-fix 77.
// ===========================================================================
BEZEL_TEST(xmod_imported_base_invariant_4592,
           imported_base_invariant_fires_at_public_method_exit_spec_22_16) {
    const int rc = buildImportAndRun4592(kInvLib4592, kInvAppMethodExit4592,
                                         checkedContractsOpts4592(),
                                         "wt-4592-mexit");
    BEZEL_EXPECT_EQ(77, rc);
}

// ===========================================================================
// Row 4 — the conforming control. §22.16's clauses are pure and a satisfied
// invariant is unobservable, so the correct program must still compute the
// right value: `new DerOk4592(5, 2)` then `Sum()` is 7. This is what makes
// rows 1-3 pins on the CHECK rather than on "the importer now rejects
// something": the re-parsed predicate reads the imported base's storage
// through the same binding the exporter compiled, and answers true.
// 7 on both sides of the fix.
// ===========================================================================
BEZEL_TEST(xmod_imported_base_invariant_4592,
           conforming_program_unaffected_by_the_check_spec_22_16) {
    const int rc = buildImportAndRun4592(kInvLib4592, kInvAppConforming4592,
                                         checkedContractsOpts4592(),
                                         "wt-4592-ok");
    BEZEL_EXPECT_EQ(7, rc);
}

// ===========================================================================
// Row 5 — the SAME-MODULE control, the row that makes this file a statement
// about the module boundary. The identical hierarchy and the identical
// violating construction, all in one module: 99 on the pre-fix compiler and 99
// on the fixed one. §2.8.2 clause 2's requirement is exactly this equality —
// an imported declaration must behave as a same-module one does — and rows 1
// and 5 disagreeing by 42-vs-99 was the whole defect.
// ===========================================================================
BEZEL_TEST(xmod_imported_base_invariant_4592,
           same_module_control_already_fired_spec_22_16) {
    const int rc = buildImportAndRun4592(/*libSrc=*/"", kInvLocalControl4592,
                                         checkedContractsOpts4592(),
                                         "wt-4592-local");
    BEZEL_EXPECT_EQ(99, rc);
}

// ===========================================================================
// Row 6 — the MODE control (§22.16 / #4386). "`ensures` and `invariant` run
// under checked-contracts mode"; in unchecked (release) mode — the default —
// the clause is not evaluated at any instrumentation point. Row 1's program,
// built with a default `CodegenOptions`, must therefore run to completion and
// return 42. Without this row the fix could have emitted the imported chain
// unconditionally and rows 1-3 would not have noticed.
// 42 on both sides of the fix.
// ===========================================================================
BEZEL_TEST(xmod_imported_base_invariant_4592,
           unchecked_mode_still_elides_the_imported_check_spec_22_16) {
    const codegen::CodegenOptions release;  // §22.16 unchecked (release) mode
    const int rc = buildImportAndRun4592(kInvLib4592, kInvAppDerived4592,
                                         release, "wt-4592-release");
    BEZEL_EXPECT_EQ(42, rc);
}

}  // namespace bezel::module
