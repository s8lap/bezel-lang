// Regression tests for #4599 — the two residuals #4592 left open in
// evaluating an IMPORTED class's §22.16 invariants across a module boundary.
//
// BACKGROUND. #4592 (tests/module/xmod_imported_base_invariant_4592_test.cpp)
// made `ModuleLoader::parseAndAttachInvariants` re-parse a TOP-LEVEL imported
// class's OWN `invariant(E);` clauses from `MetaTypeDecl::ownInvariantText`
// (the §2.8.2 display-text channel) in a synthetic re-declaration of the
// class, so `Emitter::emitInvariantCheck` (which cannot see an imported
// class's AST at all — `TypeSymbol::decl` is null for every import) has
// something to evaluate. Two shapes rode past that fix unclosed:
//
//   * RESIDUAL 2 — a §11.6 NESTED imported class's clauses. The pass-3b loop
//     that calls `parseAndAttachInvariants` walked `md.types` — TOP-LEVEL
//     types only — so a nested class's `ownInvariantText`, though carried on
//     the wire identically to a top-level one's (`convertTypeDecl` populates
//     it uniformly), was never re-parsed.
//
//   * RESIDUAL 1 — a clause naming a member INHERITED from a base class. The
//     synthetic source `buildSyntheticFieldInitSource` re-declared only the
//     class's OWN members with no base clause at all, so an unqualified
//     reference to a base field failed to resolve in the fresh context and
//     the reconstruction silently declined (the §14.1/#783 soundness gate:
//     "a clause that does not type-check cleanly ... decline instead").
//
// AUTHORITY.
//
// spec §22.16 (v0.306), "Class invariants":
//     "The class invariant of C is the conjunction of every invariant member
//      of C and of every base class, evaluated base-first."
//
// spec §11.6 (v0.286/v0.306), "Nested types":
//     "A class body may declare nested classes, records, enums, and
//      interfaces. ... Nested types may themselves contain nested types."
//
// spec §2.8.2 clause 2 requires an imported declaration to behave as a
// same-module one does — the property both residuals violated (a nested or
// base-referencing invariant that fires locally was silently skipped across
// the boundary).
//
// THE FIX (ModuleLoader.cpp).
//
//   * Residual 2: `parseAndAttachInvariantsNested` recurses through
//     `td.members[].nestedType`, sync-matched against the already-
//     reconstructed `NestedTypeSymbol::type` (pass 2 populates it before
//     pass 3b runs), and calls `parseAndAttachInvariants` on every nested
//     CLASS with a non-empty `ownInvariantText`, recursively.
//
//   * Residual 1: `parseAndAttachInvariants` now finds the REAL, already-
//     fully-resolved base `TypeSymbol*` (`importedType.baseTypes`, the same
//     field `Emitter::baseClassOf` reads), registers it into the fresh
//     `SemaContext` under ITS OWN declaring module (which may differ from
//     the class's own module — a base reached transitively through another
//     import), and gives the synthetic header a bare ` : Base` clause. Member
//     lookup then walks the REAL base chain (not a re-declared stub), so an
//     inherited field's resolved symbol already IS the real `FieldSymbol` —
//     no remap needed, the existing "#2375 sibling member" fallback arm
//     handles it unmodified.
//
//     MEASURED SCOPE (narrower than the original filing guessed — found by
//     driving a real build, not by inspection). A GENERIC base is not
//     covered (the bare name cannot supply its type argument). More
//     surprisingly, a base whose EVERY constructor takes a parameter is
//     ALSO not covered: the synthetic class declares no constructor of its
//     own (as it never has, for any caller of this source builder), so §12.8
//     demands its implicit default constructor's implicit `base()` chain
//     resolve, even though nothing in the throwaway module is ever
//     instantiated — "implicit `base()` chain: base class `X` has no
//     parameterless constructor; the chain must be explicit (§12.8)".
//     Closing that needs a forwarding constructor whose parameters
//     type-match the base's, which for a non-primitive parameter needs that
//     type's OWN declaration reconstructed too — the same cross-type textual
//     surface #710/#2290 already tracks, reached through the implicit-chain
//     check instead of #710's filed cross-type-call-in-a-ctor-body repro.
//     Both gaps decline via the ordinary resolveTypes/typecheck soundness
//     gate — the SAME pre-fix behaviour for those narrower shapes, not a
//     regression. See `parseAndAttachInvariants`'s own header comment for
//     the full account.
//
// THE TEST. Every row crosses a REAL `--import` boundary (an in-module shape
// was never broken — see the #4592 same-module control this file does not
// repeat). Uses the established two/three-artifact hand-roll
// (`Driver::compileObject` + `compileFileForTest` + `runProgramForTest`),
// mirroring xmod_imported_base_invariant_4592_test.cpp's harness and
// xmod_cross_artifact_nested_type_in_signature_3581_test.cpp's three-artifact
// chain (§2.8.3 rule 1: the app's deps list carries the FULL transitive
// closure explicitly).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
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

struct TempFile4599 {
    std::string path;
    explicit TempFile4599(std::string p) : path(std::move(p)) {}
    ~TempFile4599() { std::remove(path.c_str()); }
};

void writeSource4599(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build each of `libSrcs` in order as a `-c` library OBJECT (each one built
// against every PRIOR library already in `deps`, so a chain — lib1, lib2
// importing lib1, ... — resolves exactly as #3581's three-artifact pattern
// does), then build `mainSrc` as an executable against the FULL accumulated
// dependency list, link, run, and return the program's exit code.
//
// Distinct negative sentinels (mirroring #4592's own harness), so a broken
// scaffold can never read as a pass:
//   -2  a library did not build (or produced no object)
//   -3  the app did not build/link
//   -4  the program could not be run to a normal exit
int buildChainAndRun4599(const std::vector<std::string>& libSrcs,
                         const std::string& mainSrc,
                         const codegen::CodegenOptions& opts, const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    std::vector<std::unique_ptr<TempFile4599>> keep;
    std::vector<bezel::Dependency> deps;
    bezel::Driver driver;

    for (std::size_t i = 0; i < libSrcs.size(); ++i) {
        auto srcF = std::make_unique<TempFile4599>(
            (dir / (id + "_lib" + std::to_string(i) + ".bzl")).string());
        auto objF = std::make_unique<TempFile4599>(
            (dir / (id + "_lib" + std::to_string(i) + ".o")).string());
        writeSource4599(srcF->path, libSrcs[i]);
        // compileObject derives the library build's options from `opts`,
        // forcing isMainModule=false / sanitizeAddress=false (Driver.hpp) —
        // §22.16's checked-contracts bit still carries through, which is
        // what makes the boundary meaningful for these rows.
        if (!driver.compileObject(srcF->path, objF->path, deps, opts).success)
            return -2;
        if (!std::ifstream(objF->path).good()) return -2;
        bezel::Dependency d;
        d.metadataPath = objF->path;
        d.objectPath = objF->path;
        deps.push_back(d);
        keep.push_back(std::move(srcF));
        keep.push_back(std::move(objF));
    }

    TempFile4599 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4599 appExeF((dir / id).string());
    writeSource4599(appSrcF.path, mainSrc);

    if (!bezel::test::compileFileForTest(driver, appSrcF.path, appExeF.path,
                                         deps, opts)
             .success)
        return -3;

    int status = bezel::test::runProgramForTest(appExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -4;
}

codegen::CodegenOptions checkedContractsOpts4599() {
    codegen::CodegenOptions o;
    o.checkedContracts = true;  // exactly what `bezelc --checked-contracts` sets
    return o;
}

// ── RESIDUAL 2 fixtures: a §11.6 NESTED imported class's own invariant ────
//
// `Outer4599N` has no invariant of its own; `Inner4599N`, nested inside it,
// declares `invariant(v > 0);` and stores whatever it is given so a
// violating construction reaches construction exit rather than being
// rejected inside the exporter.
const char* kNestLib4599 =
    "module NestLib4599; "
    "public class Outer4599N "
    "  public class Inner4599N "
    "    public v: int := 1; "
    "    invariant(v > 0); "
    "    public constructor(x: int) v := x; end constructor; "
    "    public method Get(): int => v; "
    "  end Inner4599N; "
    "end Outer4599N; ";

const char* kNestAppViolate4599 =
    "module NestApp4599; "
    "import NestLib4599; "
    "class MainNV4599 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var d := new Outer4599N.Inner4599N(-1); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainNV4599; ";

const char* kNestAppConform4599 =
    "module NestAppOk4599; "
    "import NestLib4599; "
    "class MainNC4599 "
    "  public static method Main(): int "
    "    using var d := new Outer4599N.Inner4599N(5); "
    "    return d.Get(); "
    "  end Main; "
    "end MainNC4599; ";

// ── RESIDUAL 1 fixtures (SAME-module base): the derived class's OWN clause
// names a field declared on its base, both exported from the same library.
//
// `BaseS4599`'s constructor is deliberately PARAMETERLESS (measured scope —
// see the header comment and `parseAndAttachInvariants`'s own): the
// synthetic reconstruction declares no constructor of its own, so §12.8
// requires its implicit default constructor's implicit `base()` chain to
// resolve, which only a parameterless (or absent) base constructor permits.
// `DerS4599`'s OWN real constructor assigns the inherited field directly
// (no explicit `base(...)` call needed, since the base ctor takes nothing).
const char* kInhLibSame4599 =
    "module InhLibSame4599; "
    "public class BaseS4599 "
    "  public BF: int := 1; "
    "  public constructor() end constructor; "
    "end BaseS4599; "
    "public class DerS4599 : BaseS4599 "
    "  invariant(BF > 0); "
    "  public constructor(x: int) BF := x; end constructor; "
    "  public method Get(): int => BF; "
    "end DerS4599; ";

const char* kInhAppSameViolate4599 =
    "module InhAppSame4599; "
    "import InhLibSame4599; "
    "class MainISV4599 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var d := new DerS4599(-1); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainISV4599; ";

const char* kInhAppSameConform4599 =
    "module InhAppSameOk4599; "
    "import InhLibSame4599; "
    "class MainISC4599 "
    "  public static method Main(): int "
    "    using var d := new DerS4599(7); "
    "    return d.Get(); "
    "  end Main; "
    "end MainISC4599; ";

// ── RESIDUAL 1 fixtures (CROSS-module base): the base and the derived class
// are exported from TWO DIFFERENT libraries — the shape that specifically
// exercises importing the base's OWN declaring module (which differs from
// the derived class's), not just the #2375 same-module-sibling path.
// `BaseX4599`'s constructor is parameterless for the same §12.8 reason
// `kInhLibSame4599` above is.
const char* kInhLibBase4599 =
    "module InhLibBase4599; "
    "public class BaseX4599 "
    "  public BX: int := 1; "
    "  public constructor() end constructor; "
    "end BaseX4599; ";

const char* kInhLibMid4599 =
    "module InhLibMid4599; "
    "import InhLibBase4599; "
    "public class MidX4599 : BaseX4599 "
    "  invariant(BX > 0); "
    "  public constructor(x: int) BX := x; end constructor; "
    "  public method Get(): int => BX; "
    "end MidX4599; ";

const char* kInhAppCrossViolate4599 =
    "module InhAppCross4599; "
    "import InhLibMid4599; "
    "class MainICV4599 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var d := new MidX4599(-1); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainICV4599; ";

const char* kInhAppCrossConform4599 =
    "module InhAppCrossOk4599; "
    "import InhLibMid4599; "
    "class MainICC4599 "
    "  public static method Main(): int "
    "    using var d := new MidX4599(9); "
    "    return d.Get(); "
    "  end Main; "
    "end MainICC4599; ";

}  // namespace

// ===========================================================================
// RESIDUAL 2 — a §11.6 NESTED imported class's own invariant.
//
// Pre-fix `parseAndAttachInvariantsNested` does not exist and pass 3b's loop
// walks `md.types` (top level) only, so `Outer4599N` — whose OWN
// `ownInvariantText` is empty — is skipped by its `continue`, and
// `Inner4599N` is never visited at all: `Inner4599N.importedInvariantExprs`
// stays empty and `emitInvariantCheck` finds nothing to check. Construction
// with v = -1 completes silently. Post-fix the recursion reaches
// `Inner4599N` and the violation fires. Measured pre-fix 42, post-fix 99.
// ===========================================================================
BEZEL_TEST(xmod_invariant_nested_and_inherited_4599,
           nested_imported_class_invariant_fires_at_construction_exit_spec_22_16_11_6) {
    const int rc = buildChainAndRun4599({kNestLib4599}, kNestAppViolate4599,
                                        checkedContractsOpts4599(),
                                        "wt-4599-nest-v");
    BEZEL_EXPECT_EQ(99, rc);
}

// Mode control: unchecked (release, the default) mode must NOT evaluate the
// clause — §22.16's instrumentation is checked-contracts-only. 42 on both
// sides of the fix; without this row the fix could unconditionally emit the
// nested chain and this file would not notice.
BEZEL_TEST(xmod_invariant_nested_and_inherited_4599,
           nested_imported_class_invariant_unchecked_mode_control_spec_22_16) {
    const codegen::CodegenOptions release;
    const int rc = buildChainAndRun4599({kNestLib4599}, kNestAppViolate4599,
                                        release, "wt-4599-nest-rel");
    BEZEL_EXPECT_EQ(42, rc);
}

// Conforming control: a satisfied invariant is unobservable (§22.16 is pure),
// so construction must complete and the field must read back through the
// SAME binding the exporter compiled. 5 on both sides (this shape was never
// rejected pre-fix; it pins that the fix does not disturb the happy path).
BEZEL_TEST(xmod_invariant_nested_and_inherited_4599,
           nested_imported_class_invariant_conforming_construction_spec_22_16) {
    const int rc = buildChainAndRun4599({kNestLib4599}, kNestAppConform4599,
                                        checkedContractsOpts4599(),
                                        "wt-4599-nest-ok");
    BEZEL_EXPECT_EQ(5, rc);
}

// ===========================================================================
// RESIDUAL 1 (same-module base) — the derived class's OWN clause names a
// field declared on its base, both exported from ONE library. `BaseS4599`
// has a PARAMETERLESS constructor — the measured scope this fix closes; see
// the header comment and `parseAndAttachInvariants`'s own for why a base
// whose every constructor takes a parameter is a DIFFERENT, still-open gap.
//
// Pre-fix the synthetic source for `DerS4599` declares no base clause at
// all, so `BF` fails to resolve in the fresh context, `typecheckExpressions`
// reports an error, and the §14.1/#783 soundness gate declines — leaving
// `DerS4599.importedInvariantExprs` empty (the pre-#4592 behaviour for this
// shape) and BaseS4599 itself carries no invariant of its own for the
// base-first chain to fall back to, so nothing fires. Post-fix the base
// clause resolves against the REAL BaseS4599 symbol and the violation fires.
// Measured pre-fix 42, post-fix 99.
// ===========================================================================
BEZEL_TEST(xmod_invariant_nested_and_inherited_4599,
           inherited_member_same_module_base_invariant_fires_spec_22_16) {
    const int rc = buildChainAndRun4599({kInhLibSame4599},
                                        kInhAppSameViolate4599,
                                        checkedContractsOpts4599(),
                                        "wt-4599-inh-same-v");
    BEZEL_EXPECT_EQ(99, rc);
}

BEZEL_TEST(xmod_invariant_nested_and_inherited_4599,
           inherited_member_same_module_base_unchecked_mode_control_spec_22_16) {
    const codegen::CodegenOptions release;
    const int rc = buildChainAndRun4599({kInhLibSame4599},
                                        kInhAppSameViolate4599, release,
                                        "wt-4599-inh-same-rel");
    BEZEL_EXPECT_EQ(42, rc);
}

BEZEL_TEST(xmod_invariant_nested_and_inherited_4599,
           inherited_member_same_module_base_conforming_construction_spec_22_16) {
    const int rc = buildChainAndRun4599({kInhLibSame4599},
                                        kInhAppSameConform4599,
                                        checkedContractsOpts4599(),
                                        "wt-4599-inh-same-ok");
    BEZEL_EXPECT_EQ(7, rc);
}

// ===========================================================================
// RESIDUAL 1 (CROSS-module base) — `MidX4599`'s base `BaseX4599` is exported
// from a DIFFERENT library than `MidX4599` itself, imported only
// transitively (the app imports `InhLibMid4599` alone; `InhLibBase4599`'s
// object is supplied per §2.8.3 rule 1's transitive-closure requirement,
// mirroring xmod_cross_artifact_nested_type_in_signature_3581_test.cpp's
// three-artifact pattern). This is the shape that specifically exercises
// `buildSyntheticFieldInitSource`'s second `import <baseSym->moduleName>;`
// line — the same-module row above alone could pass with only the #2375
// sibling-registration path reused, never touching the new import line.
// Measured pre-fix 42, post-fix 99.
// ===========================================================================
BEZEL_TEST(xmod_invariant_nested_and_inherited_4599,
           inherited_member_cross_module_base_invariant_fires_spec_22_16) {
    const int rc = buildChainAndRun4599({kInhLibBase4599, kInhLibMid4599},
                                        kInhAppCrossViolate4599,
                                        checkedContractsOpts4599(),
                                        "wt-4599-inh-cross-v");
    BEZEL_EXPECT_EQ(99, rc);
}

BEZEL_TEST(xmod_invariant_nested_and_inherited_4599,
           inherited_member_cross_module_base_conforming_construction_spec_22_16) {
    const int rc = buildChainAndRun4599({kInhLibBase4599, kInhLibMid4599},
                                        kInhAppCrossConform4599,
                                        checkedContractsOpts4599(),
                                        "wt-4599-inh-cross-ok");
    BEZEL_EXPECT_EQ(9, rc);
}

// ===========================================================================
// RESIDUAL 1's OWN measured boundary — a base whose only constructor takes a
// parameter STILL declines, on BOTH sides of this fix. This is not the
// original residual-1 shape (that one now fires, see above); it is the
// narrower gap the fix's own investigation found and left open (see
// `parseAndAttachInvariants`'s header, "NOT covered"): §12.8's implicit
// `base()` chain requires a parameterless (or absent) base constructor for
// the synthetic class, which declares none of its own. Constant on both
// sides of this fix (42 pre-fix AND post-fix) — a regression test in the
// OTHER direction, pinning that the fix does not silently misfire (e.g. by
// evaluating the clause against garbage) when it cannot fully reconstruct.
// Filed as #710/#2290's residual per this fix's commit message, not solved
// here.
// ===========================================================================
namespace {
const char* kInhLibParamBase4599 =
    "module InhLibParamBase4599; "
    "public class BaseP4599 "
    "  public BP: int := 1; "
    "  public constructor(x: int) BP := x; end constructor; "
    "end BaseP4599; "
    "public class DerP4599 : BaseP4599 "
    "  invariant(BP > 0); "
    "  public constructor(x: int) base(x); end constructor; "
    "end DerP4599; ";

const char* kInhAppParamBaseViolate4599 =
    "module InhAppParamBase4599; "
    "import InhLibParamBase4599; "
    "class MainIPV4599 "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    try "
    "      using var d := new DerP4599(-1); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end MainIPV4599; ";
}  // namespace

BEZEL_TEST(xmod_invariant_nested_and_inherited_4599,
           inherited_member_parameterized_base_ctor_still_declines_spec_12_8_710) {
    const int rc = buildChainAndRun4599({kInhLibParamBase4599},
                                        kInhAppParamBaseViolate4599,
                                        checkedContractsOpts4599(),
                                        "wt-4599-inh-paramctor");
    BEZEL_EXPECT_EQ(42, rc);
}

}  // namespace bezel::module
