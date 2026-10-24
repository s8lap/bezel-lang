// #7165 (§22.5, stdlib §9.6, v0.343) — the static `string.Join` CODEGEN arm
// must bind a NAMED argument to the PARAMETER it names.
//
// WHAT IS BROKEN. stdlib §9.6 declares
//
//     public static method Join(separator: string, items: IEnumerable<string>): string;
//
// and §22.5 says "named arguments may appear in any order after positional
// arguments. Each supplied argument binds exactly one parameter". Sema binds
// the named arguments correctly and PUBLISHES that binding (the
// `paramToArg` vector: parameter index -> source argument index; see
// `ExprTC::checkBuiltinNamedArgs` -> `SemaContext::setBuiltinCallBinding`).
// The codegen arm for the static `string.Join` did not read it: it took the
// separator from `call.args[0]` and the items from `call.args[1]` — the
// POSITIONS — so `string.Join(items := parts, separator := ",")` bound the
// separator to the ITEMS argument. The arm is fixed in
// `src/codegen/LlvmBackend_Calls.cpp` (the static `string.Join` arm): it
// now reads the source argument index of the `separator` PARAMETER and of the
// `items` PARAMETER from the published binding, falling back to the
// positional read when there is no binding (a positional call, or a named
// call already in declaration order).
//
// WHY THIS ROW IS NOT A PLAIN runAot ROW. On this tree the reordered call is
// REJECTED by the front end: the #7150 separator gate
// (`sema/ExprTypecheck_Calls.cpp`, `checkAssignable(..., "separator argument
// to `string.Join`", ...)`) reads `ce.args[0]` POSITIONALLY, so it sees the
// ITEMS argument where it expects the separator and reports "the separator
// argument to `string.Join` has a type that is not implicitly convertible to
// the target type". That gate is the SEMANTIC half of #7165 and belongs to
// #7164; this lane owns the CODEGEN half only, and the gate is NOT weakened
// here — it still rejects the call. But a program the front end rejects never
// reaches codegen through the shipping driver, so the row below drives the
// front end BY HAND and observes the CODEGEN arm itself. The reordered call
// is deliberately NOT pinned end-to-end: `tests/sema/
// string_join_separator_type_gate_7150_test.cpp` leaves
// `string.Join(items := parts, separator := ",")` unpinned until #7164's
// semantic half lands, and this row keeps it unpinned too.
//
// THE VALUE, NOT THE EXIT CODE. Every row here asserts the program's STDOUT
// (the VALUE channel). A process exit status is masked to 0..255 and cannot
// carry a string value, so an exit-code assertion could not tell "a,b,c" from
// "b,c,a" or from most other wrong answers.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/emit_object_support.hpp"
#include "support/force_sanitize.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

// The reordered call: the separator is the SECOND argument, so an arm that
// reads `call.args[0]` as the separator computes the wrong value. Both
// arguments are locals, so lowering them twice (the generic named-argument
// pre-pass in `LlvmBackend_Calls2.cpp` lowers each supplied argument
// expression before the arm runs) allocates nothing twice.
const char* kJoinReorderedNamed =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var parts: Array<string> := [\"a\", \"b\", \"c\"]; "
    "    var sep: string := \",\"; "
    "    Console.WriteLine(string.Join(items := parts, separator := sep)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The same call with the arguments in DECLARATION order (the binding is the
// identity permutation, so sema publishes nothing and the arm reads
// positionally) — the control for the fallback path.
const char* kJoinNamedInOrder =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var parts: Array<string> := [\"a\", \"b\", \"c\"]; "
    "    Console.WriteLine(string.Join(separator := \",\", items := parts)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The positional call — the control for the arm's positional fallback.
const char* kJoinPositional =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var parts: Array<string> := [\"a\", \"b\", \"c\"]; "
    "    Console.WriteLine(string.Join(\",\", parts)); "
    "    return 0; "
    "  end Main; "
    "end C;";

struct DirectRun {
    bool built = false;      // the front end + emit + link all succeeded
    std::string stage;       // which step failed ("" when built)
    std::string stdoutText;  // the program's stdout, one trailing newline stripped
    std::string stderrText;  // the program's stderr (sanitizer reports land here)
    int exitCode = -1;       // informational only — never the assertion
};

// Drive the CODEGEN arm directly: run the front end by hand, emit the module,
// link it with the shipping link recipe, run it, and capture its stdout.
//
// The front end's diagnostics are deliberately NOT treated as a failure here:
// the #7150 separator gate rejects the reordered call (see the header), and
// that rejection belongs to #7164's semantic half. This row characterizes the
// CODEGEN arm, which is this lane's half of #7165. The gate itself is
// untouched — it still rejects the call through the shipping driver.
//
// The object is emitted through `emitObjectFileForTest`, so the whole-suite
// ASan sweep reaches this row's generated code; the link uses the driver's own
// recipe, which adds `-fsanitize=address` under the sweep.
DirectRun runCodegenArm(const std::string& source) {
    DirectRun out;
    auto dir = wtScratchDir("wt-join-7165");
    std::string stem = unique_stem("bezel_join_7165");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exePath = (dir / stem).string();
    std::string outPath = (dir / (stem + ".out")).string();
    std::string errPath = (dir / (stem + ".err")).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) { out.stage = "parse"; return out; }
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        const std::size_t diagMark = diags.diagnostics().size();
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        // The front end reports the #7150 separator gate here (see the header).
        // The report is ROLLED BACK — not suppressed at its source: the gate
        // still runs and still reports, and the shipping driver still rejects
        // the reordered call. It is dropped here only because the emitter
        // refuses to emit a module for a compilation that carries an Error
        // (`Emitter::run` returns nullptr), and this row needs the CODEGEN
        // arm to be exercised in isolation. Nothing here pins the rejection:
        // when #7164's semantic half lands the front end reports nothing,
        // the rollback drops nothing, and the row still holds.
        diags.rollbackTo(diagMark);
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) { out.stage = "emitModule"; return out; }
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) {
            out.stage = "emitObjectFile: " + err;
            return out;
        }
    }
    bezel::codegen::CodegenOptions opts;
    opts.checkedContracts = true;
    opts.sanitizeAddress = bezel::test::forceSanitizeAddress();
    bezel::Driver driver;
    if (!driver.linkExecutableObject(objPath, exePath, /*deps=*/{}, opts)) {
        out.stage = "link";
        return out;
    }
    std::string cmd = exePath + " > " + outPath + " 2> " + errPath;
    int status = std::system(cmd.c_str());
    if (status != -1 && WIFEXITED(status)) out.exitCode = WEXITSTATUS(status);
    {
        std::ifstream f(outPath);
        std::ostringstream ss;
        ss << f.rdbuf();
        out.stdoutText = ss.str();
    }
    {
        std::ifstream f(errPath);
        std::ostringstream ss;
        ss << f.rdbuf();
        out.stderrText = ss.str();
    }
    std::remove(objPath.c_str());
    std::remove(exePath.c_str());
    std::remove(outPath.c_str());
    std::remove(errPath.c_str());
    out.built = true;
    return out;
}

// Strip the single trailing line ending Console.WriteLine appends
// (Console.NewLine — "\n" on Unix, "\r\n" on Windows), so the expectation is
// the bare value and stays platform-independent.
std::string stripTrailingNewline(std::string s) {
    if (!s.empty() && s.back() == '\n') {
        s.pop_back();
        if (!s.empty() && s.back() == '\r') s.pop_back();
    }
    return s;
}

}  // namespace

// The regression row: the reordered named call must join with the separator
// it NAMES. Before the fix the arm read `call.args[0]` as the separator, so
// this program printed the wrong value instead of "a,b,c".
BEZEL_TEST(codegen_string_join_named_arg_binding_7165,
           reordered_named_args_bind_by_parameter_name_22_5) {
    DirectRun r = runCodegenArm(kJoinReorderedNamed);
    if (!r.built) std::fprintf(stderr, "  [stage] %s\n", r.stage.c_str());
    std::string got = stripTrailingNewline(r.stdoutText);
    if (got != "a,b,c")
        std::fprintf(stderr, "  [stdout] \"%s\" (exit %d)\n", got.c_str(),
                     r.exitCode);
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(got, std::string("a,b,c"));
}

// Control: the named call in DECLARATION order (identity binding — sema
// publishes nothing, the arm reads positionally) keeps its value.
BEZEL_TEST(codegen_string_join_named_arg_binding_7165,
           named_args_in_declaration_order_unchanged_9_6) {
    DirectRun r = runCodegenArm(kJoinNamedInOrder);
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(stripTrailingNewline(r.stdoutText), std::string("a,b,c"));
}

// Control: the positional call keeps its value.
BEZEL_TEST(codegen_string_join_named_arg_binding_7165,
           positional_call_unchanged_9_6) {
    DirectRun r = runCodegenArm(kJoinPositional);
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(stripTrailingNewline(r.stdoutText), std::string("a,b,c"));
}
