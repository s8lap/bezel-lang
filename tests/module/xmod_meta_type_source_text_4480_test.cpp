// Regression tests for #4480 — §27.1 / §3.1 / §15.1 / Appendix A.5 (v0.306):
// `metaTypeToSourceText` must RENDER a function, inline-array or unit type
// rather than substituting the placeholder `Object`.
//
// THE DEFECT. `metaTypeToSourceText` (src/module/ModuleLoader.cpp) handled
// exactly two of the six `TypeKind`s — Primitive and Named — and ended:
//
//     // Fallback for unsupported shapes (Function, InlineArray): use a
//     // placeholder.
//     return "Object";
//
// That function builds the SYNTHETIC RECONSTRUCTION SOURCE an importer
// re-parses: the field-initializer replay (#2874/#3671/#3723), the ctor/dtor
// body re-parse (#662c), the base-clause argument re-parse (#3911). A library
// exporting
//
//     public record Box(N: int)
//       public F: (int) -> int := lambda(x) => x + 7;
//     end Box;
//
// was therefore reconstructed on the consumer side as `public F : Object :=
// lambda(x) => x + 7;` — the declared type replaced by one the exporter never
// wrote, before the initializer is re-typed and re-run.
//
// WHY IT LOOKED HARMLESS. For a function type three mitigations happen to hold
// at once: it is a reference type deriving from `Object` (§3), so the
// substitution is a widening rather than a contradiction; the replayed lambda
// usually re-announces its OWN parameter types (`lambda(x: int) => …`), so
// inference recovers a real function type from the initializer instead of from
// the declaration; and the storage slot is a `ptr` either way. Remove ANY one
// and the replay runs against a type the exporter never declared — which is
// what rows 1 and 2 do, by writing the lambda in the parameter-type-inferring
// spelling §27.3 admits (`lambda(x) => x + 7`, types "inferred from the target
// type"). With the target type erased to `Object` there is nothing to infer
// from, and the importer REJECTS a program the exporter compiled.
//
// THE FIX (this commit) renders the three renderable shapes from the wire data
// the metadata already carries, per Appendix A.5:
//   FunctionType    ::= "(" (FuncParamType …)? ")" "->" "owned"? Type
//   FuncParamType   ::= FuncParamMods? (Identifier ":")? Type
//   InlineArrayType ::= "[" ConstIntExpr "owned"? "]" Type
//   TypeNoNullable  ::= … | "(" ")"                       /* the unit type */
// Parameter MODIFIERS are rendered because §27.1 makes them part of the type
// ("`(T) -> ()` and `(owned T) -> ()` are distinct function types"); parameter
// NAMES are not, because §27.1's own table spells every function type without
// them. A nullable function type is parenthesised per §27.2.
//
// MEASURED ACROSS THE FIX BOUNDARY (real `bezelc`, a `-c` library plus an
// importing executable — see the commit body for the commands):
//   row 1 shape   pre-fix: "constructing imported record `RD4480` cannot run
//                           the initializer of its field `F`"   (compile error)
//                 post-fix: exit 42
//   row 2 shape   pre-fix: same rejection on field `F`
//                 post-fix: exit 42
//   row 3 shape   42 on both — the mitigation the issue names, which must keep
//                 working.
//
// INLINE ARRAYS, MEASURED AND REPORTED RATHER THAN PINNED. The issue flagged
// `[N]T` as the shape where none of the three mitigations apply, and that is
// correct — but the end-to-end shape that would show it (`public Items: [2]int
// := [1, 2];` on an exported record) is blocked one stage further on, in
// codegen: the SINGLE-MODULE twin of that program fails with the identical
// "internal: lowerExpr returned null for record field/property default
// initializer — #1386". With this fix the cross-module path stops deciding the
// outcome and converges on the in-module one; no row is added here, because a
// row asserting that failure would pin the unrelated lowering gap.
//
// THE TEST. Every row crosses a real `--import` boundary — the placeholder is
// only emitted on the reconstruction path, so an in-module test cannot reach
// it. The AOT harness compiles one module and cannot express a dependency, so
// the rows hand-roll `Driver::compileObject` plus
// `bezel::test::compileFileForTest`, the established two-module shape. No JIT
// oracle is used.

#include <cstdio>
#include <cstdlib>
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

struct TempFile4480 {
    std::string path;
    explicit TempFile4480(std::string p) : path(std::move(p)) {}
    ~TempFile4480() { std::remove(path.c_str()); }
};

void writeSource4480(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build `libSrc` as a `-c` library object, build `mainSrc` as an executable
// importing it, run it, return the exit code. Distinct negative sentinels so a
// scaffold failure can never read as a pass; every row asserts an exact
// non-negative value.
//   -2  the library half did not build
//   -3  the importer half did not build (this is the PRE-FIX result of rows 1/2)
//   -4  the program could not be run to a normal exit
int buildImportAndRun4480(const std::string& libSrc, const std::string& mainSrc,
                          const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4480 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4480 libObjF((dir / (id + "_lib.o")).string());
    TempFile4480 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4480 appExeF((dir / id).string());
    writeSource4480(libSrcF.path, libSrc);
    writeSource4480(appSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    // compileFileForTest, not compileFile, so `gate.sh --full`'s
    // BEZEL_AOT_FORCE_SANITIZE sweep reaches the importer half (#3653).
    if (!bezel::test::compileFileForTest(driver, appSrcF.path, appExeF.path,
                                          deps, codegen::CodegenOptions{})
             .success)
        return -3;

    int status = std::system(appExeF.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -4;
}

// ── Row 1: `(int) -> int`, inferred lambda parameter type ────────────────
// §27.3: `lambda(x) => x * 2` takes its parameter types "from the target
// type". The target type here is the FIELD's declared type, which is what the
// reconstruction source has to carry across the boundary.
const char* kLibFn4480 =
    "module LibFn4480; "
    "public record RFn4480(N: int) "
    "  public F: (int) -> int := lambda(x) => x + 7; "
    "end RFn4480; ";

const char* kAppFn4480 =
    "module AppFn4480; "
    "import LibFn4480; "
    "class AppFn4480Main "
    "  public static method Main(): int "
    "    var a := new RFn4480(5); "
    "    return a.F(a.N) + 30; "   // 5 + 7 + 30 == 42
    "  end Main; "
    "end AppFn4480Main; ";

// ── Row 2: `(ref int) -> ()`, a MODIFIER and a UNIT return ───────────────
// Two renderings in one type: §27.1's identity-bearing `ref` on the parameter
// slot, and A.5's `"(" ")"` unit spelling as the return. `lambda(ref x)` again
// leaves the parameter type to be inferred from the declared target type.
const char* kLibRef4480 =
    "module LibRef4480; "
    "public record RRef4480(N: int) "
    "  public F: (ref int) -> () := lambda(ref x) => do x := x + 1; end; "
    "end RRef4480; ";

const char* kAppRef4480 =
    "module AppRef4480; "
    "import LibRef4480; "
    "class AppRef4480Main "
    "  public static method Main(): int "
    "    var a := new RRef4480(5); "
    "    var n: int := 41; "
    "    a.F(ref n); "
    "    return n; "               // 42
    "  end Main; "
    "end AppRef4480Main; ";

// ── Row 3 (control): the annotated spelling the issue names ──────────────
// `lambda(x: int)` re-announces its own parameter type, so inference recovers
// a function type from the initializer even when the declared type was erased.
// This worked before the fix and must keep working after it.
const char* kLibAnn4480 =
    "module LibAnn4480; "
    "public record RAnn4480(N: int) "
    "  public F: (int) -> int := lambda(x: int) => x + 7; "
    "end RAnn4480; ";

const char* kAppAnn4480 =
    "module AppAnn4480; "
    "import LibAnn4480; "
    "class AppAnn4480Main "
    "  public static method Main(): int "
    "    var a := new RAnn4480(5); "
    "    return a.F(a.N) + 30; "
    "  end Main; "
    "end AppAnn4480Main; ";

}  // namespace

// ===========================================================================
// Row 1 — THE BUG (§27.1 / §27.3, v0.306). The imported record's
// function-typed field is reconstructed at its DECLARED type, so the replayed
// parameter-type-inferring lambda has a target type to infer from.
// Pre-fix the importer rejected the program ("cannot run the initializer of
// its field `F`") because the declared type had been replaced by `Object`.
// ===========================================================================
BEZEL_TEST(xmod_meta_type_source_text_4480,
           function_typed_field_replays_at_declared_type_spec_27_1) {
    const int rc = buildImportAndRun4480(kLibFn4480, kAppFn4480, "wt-4480-fn");
    BEZEL_EXPECT_EQ(42, rc);
}

// ===========================================================================
// Row 2 — the modifier and the unit return (§27.1 / §3.1 / A.5, v0.306).
// `(ref int) -> ()` exercises a different part of the renderer than row 1: the
// identity-bearing parameter modifier and the `()` unit spelling. Pre-fix the
// same rejection; post-fix the closure mutates the caller's `n` through `ref`.
// ===========================================================================
BEZEL_TEST(xmod_meta_type_source_text_4480,
           function_type_modifier_and_unit_return_render_spec_27_1) {
    const int rc = buildImportAndRun4480(kLibRef4480, kAppRef4480,
                                         "wt-4480-ref");
    BEZEL_EXPECT_EQ(42, rc);
}

// ===========================================================================
// Row 3 — the control. The annotated-lambda spelling is the mitigation that
// kept the defect invisible; it passed before the fix and must still pass, so
// this row is what would catch the fix breaking the path it repairs.
// ===========================================================================
BEZEL_TEST(xmod_meta_type_source_text_4480,
           annotated_lambda_field_initializer_still_replays_spec_27_1) {
    const int rc = buildImportAndRun4480(kLibAnn4480, kAppAnn4480,
                                         "wt-4480-ann");
    BEZEL_EXPECT_EQ(42, rc);
}

}  // namespace bezel::module
