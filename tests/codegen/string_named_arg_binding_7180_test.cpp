// #7180 (spec §22.5, stdlib §9.6, v0.343) — the `string` INSTANCE method arms
// must bind a NAMED argument to the PARAMETER it names.
//
// WHAT IS BROKEN. §22.5 (v0.343) says "named arguments may appear in any
// order after positional arguments. Each supplied argument binds exactly one
// parameter" and "naming an argument changes which parameter it binds to,
// never when it runs". Sema binds a built-in member's named arguments
// correctly and PUBLISHES that binding (the `paramToArg` vector: PARAMETER
// index -> SOURCE argument index; see `ExprTC::checkBuiltinNamedArgs` ->
// `SemaContext::setBuiltinCallBinding`). The `string` instance arms in
// `src/codegen/LlvmBackend_String.cpp` did not read it: they took their
// operands from `call.args[0]` / `call.args[1]` — the POSITIONS — so a
// reordered named call handed each parameter the value of a DIFFERENT
// parameter.
//
//   * `s.Equals(ignoreCase := true, other := "ABC")` took the `i1` as the
//     `other` STRING operand and handed it to a `ptr` parameter: malformed
//     IR, and the compiler ABORTED with "IR verification failed" (#7180).
//   * `s.CompareTo(ignoreCase := true, other := "ABC")` — the same shape, and
//     the other `bool`-parameter member §9.6 declares.
//   * `s.Substring(length := 2, startIndex := 1)` took the LENGTH as the
//     START: a silently WRONG value ("c" instead of "bc").
//   * `s.Replace(newValue := "X", oldValue := "b")` took the NEW value as the
//     OLD one: a silently wrong value.
//   * `s.AsSpan(length := 2, start := 1).Length` → 1 instead of 2.
//   * `s.IndexOf(startIndex := 1, value := "c")`, `s.PadLeft(paddingChar := 'x',
//     totalWidth := 4)` and `s.Insert(value := "X", startIndex := 1)` — the
//     same positional read.
//
// The fix reads the published binding in every arity-2 `string` instance arm
// in that file (Equals, CompareTo, IndexOf, Substring, Replace, AsSpan,
// PadLeft/PadRight/PadCenter, Insert), so each PARAMETER receives the
// argument it names, and lowers the supplied argument expressions in
// CALL-SITE TEXTUAL order (§22.5: "a call's supplied argument expressions are
// evaluated in call-site textual order — left to right as written, for
// positional and named arguments alike; naming an argument changes which
// parameter it binds to, never when it runs").
//
// TWO ORACLES, AND WHY. Four of the eight members reach codegen through the
// shipping driver — `Equals`, `Substring`, `Replace` and `AsSpan` are
// accepted by the front end — so those rows are plain `runAot` rows (the
// strict oracle: the real AOT path, fork-isolated). The other four
// (`CompareTo`, `IndexOf`, `PadLeft`, `Insert`) are REJECTED by the front
// end on this tree: the #7150-family gate in `sema/ExprTypecheck_Calls.cpp`
// (`checkBuiltinStringCallArgs`) reads `ce.args` POSITIONALLY, so it sees the
// `ignoreCase` argument where it expects the `other` string and reports "the
// argument to `string.CompareTo` has a type that is not implicitly
// convertible to the target type (§5.1)". That gate is the SEMANTIC half of
// this class and lives in another lane's file; it is NOT weakened here — it
// still rejects the call. A program the front end rejects never reaches
// codegen through the shipping driver, so those rows drive the front end BY
// HAND (the #7165 house pattern) and observe the CODEGEN arm itself. The
// reordered calls of those four are deliberately NOT pinned end-to-end.
//
// THE VALUE, NOT THE EXIT CODE. Every row here asserts the program's STDOUT
// (the VALUE channel). A process exit status is masked to 0..255 and cannot
// carry a string value, so an exit-code assertion could not tell "bc" from
// "c", nor 2 from 1.
//
// THE CONTROLS. Each member also has a control row: the POSITIONAL spelling
// and the named spelling already in DECLARATION order (the identity
// permutation — sema publishes nothing for it, and the arm reads
// positionally). Those rows must be byte-identical before and after the fix:
// the fix must change the REORDERED binding and nothing else.

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
#include "support/aot_support.hpp"
#include "support/emit_object_support.hpp"
#include "support/force_sanitize.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// The programs. Every one of them prints ONE value, so the row's expectation
// is the bare value.
// ---------------------------------------------------------------------------

// §9.6: Equals(other: string?, ignoreCase: bool): bool.
// "abc" vs "ABC" is DISCRIMINATING: case-SENSITIVE they differ (False),
// case-INSENSITIVE they are equal (True) — so the value says which comparison
// the arm performed, not merely that it produced a bool. BOTH `ignoreCase`
// values are printed, so the pair pins the case-folded path and the plain
// path of the reordered call.
const char* kEqualsReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abc\"; "
    "    Console.WriteLine(s.Equals(ignoreCase := true, other := \"ABC\")); "
    "    Console.WriteLine(s.Equals(ignoreCase := false, other := \"ABC\")); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kEqualsControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abc\"; "
    "    Console.WriteLine(s.Equals(\"ABC\", true)); "
    "    Console.WriteLine(s.Equals(other := \"ABC\", ignoreCase := true)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §9.6: CompareTo(other: string, ignoreCase: bool): int — ordinal, optionally
// case-folded. "abc" vs "ABC" case-folded is 0 (equal).
const char* kCompareToReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abc\"; "
    "    Console.WriteLine(s.CompareTo(ignoreCase := true, other := \"ABC\")); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kCompareToControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abc\"; "
    "    Console.WriteLine(s.CompareTo(\"ABC\", true)); "
    "    Console.WriteLine(s.CompareTo(other := \"ABC\", ignoreCase := true)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §9.6: IndexOf(value: string, startIndex: int): int — the code-point index of
// "c" in "abcdef" from index 1 → 2.
const char* kIndexOfReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.IndexOf(startIndex := 1, value := \"c\")); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kIndexOfControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.IndexOf(\"c\", 1)); "
    "    Console.WriteLine(s.IndexOf(value := \"c\", startIndex := 1)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §9.6: Substring(startIndex: int, length: int): string — "abcdef"[1..3) →
// "bc".
const char* kSubstringReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.Substring(length := 2, startIndex := 1)); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kSubstringControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.Substring(1, 2)); "
    "    Console.WriteLine(s.Substring(startIndex := 1, length := 2)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §9.6: Replace(oldValue: string, newValue: string): string — "abcdef" with
// "b" replaced by "X" → "aXcdef".
const char* kReplaceReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.Replace(newValue := \"X\", oldValue := \"b\")); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kReplaceControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.Replace(\"b\", \"X\")); "
    "    Console.WriteLine(s.Replace(oldValue := \"b\", newValue := \"X\")); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §9.6: AsSpan(start: int, length: int): ReadOnlyStringSpan — the span's
// Length is the LENGTH parameter's code-point count → 2.
const char* kAsSpanReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.AsSpan(length := 2, start := 1).Length); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kAsSpanControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.AsSpan(1, 2).Length); "
    "    Console.WriteLine(s.AsSpan(start := 1, length := 2).Length); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §9.6: PadLeft(totalWidth: int, paddingChar: char): string — "ab" padded to
// width 4 with 'x' → "xxab".
const char* kPadLeftReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"ab\"; "
    "    Console.WriteLine(s.PadLeft(paddingChar := 'x', totalWidth := 4)); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kPadLeftControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"ab\"; "
    "    Console.WriteLine(s.PadLeft(4, 'x')); "
    "    Console.WriteLine(s.PadLeft(totalWidth := 4, paddingChar := 'x')); "
    "    return 0; "
    "  end Main; "
    "end C;";

// §9.6: Insert(startIndex: int, value: string): string — "X" inserted at
// index 1 of "abcdef" → "aXbcdef".
const char* kInsertReordered =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.Insert(value := \"X\", startIndex := 1)); "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kInsertControl =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var s: string := \"abcdef\"; "
    "    Console.WriteLine(s.Insert(1, \"X\")); "
    "    Console.WriteLine(s.Insert(startIndex := 1, value := \"X\")); "
    "    return 0; "
    "  end Main; "
    "end C;";

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

// The runAot oracle: the program's STDOUT, or "" when it did not run.
std::string aotStdout(const char* source) {
    auto r = runAot(std::string(source));
    if (!r.ran) return std::string();
    return stripTrailingNewline(r.stdoutText);
}

// ---------------------------------------------------------------------------
// The arm-level oracle, for the members the front end rejects on this tree.
//
// Drive the CODEGEN arm directly: run the front end by hand, emit the module,
// link it with the shipping link recipe, run it, and capture its stdout.
//
// The front end's diagnostics are deliberately NOT treated as a failure here:
// the #7150-family gate rejects the reordered call (see the header), and that
// rejection belongs to the semantic half of this class, in another lane's
// file. This row characterizes the CODEGEN arm, which is this lane's half of
// #7180. The gate itself is untouched — it still rejects the call through the
// shipping driver.
//
// The object is emitted through `emitObjectFileForTest`, so the whole-suite
// ASan sweep reaches this row's generated code; the link uses the driver's own
// recipe, which adds `-fsanitize=address` under the sweep.
// ---------------------------------------------------------------------------
struct DirectRun {
    bool built = false;      // the front end + emit + link all succeeded
    std::string stage;       // which step failed ("" when built)
    std::string stdoutText;  // the program's stdout, one trailing newline stripped
    std::string stderrText;  // the program's stderr (sanitizer reports land here)
    int exitCode = -1;       // informational only — never the assertion
};

DirectRun runCodegenArm(const std::string& source) {
    DirectRun out;
    auto dir = wtScratchDir("wt-str-7180");
    std::string stem = unique_stem("bezel_str_7180");
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
        // The front end reports the #7150-family positional gate here (see the
        // header). The report is ROLLED BACK — not suppressed at its source:
        // the gate still runs and still reports, and the shipping driver still
        // rejects the reordered call. It is dropped here only because the
        // emitter refuses to emit a module for a compilation that carries an
        // Error (`Emitter::run` returns nullptr), and this row needs the
        // CODEGEN arm to be exercised in isolation. Nothing here pins the
        // rejection: when the semantic half lands the front end reports
        // nothing, the rollback drops nothing, and the row still holds.
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

}  // namespace

// ===========================================================================
// The rows the front end accepts — the strict AOT oracle.
// ===========================================================================

// THE REGRESSION ROW (#7180). Before the fix the compiler ABORTED on this
// program ("IR verification failed"): the `i1` reached the `ptr` parameter.
// Now the reordered call must bind the `other` PARAMETER to the string
// operand and the `ignoreCase` PARAMETER to the bool: "abc" vs "ABC" is True
// case-insensitively and False case-sensitively, so the two printed values
// say WHICH comparison ran — a swapped binding would swap them.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           equals_reordered_named_args_bind_by_parameter_name_22_5) {
    std::string got = aotStdout(kEqualsReordered);
    if (got != "True\nFalse")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("True\nFalse"));
}

// Control: the POSITIONAL spelling and the named spelling already in
// DECLARATION order keep their values, byte for byte.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           equals_positional_and_in_order_named_unchanged_9_6) {
    std::string got = aotStdout(kEqualsControl);
    if (got != "True\nTrue")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("True\nTrue"));
}

// The reordered `Substring` must take the START from the `startIndex`
// PARAMETER and the COUNT from the `length` PARAMETER: "abcdef"[1..3) is
// "bc". Before the fix the arm took the LENGTH as the start → "c".
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           substring_reordered_named_args_bind_by_parameter_name_22_5) {
    std::string got = aotStdout(kSubstringReordered);
    if (got != "bc")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("bc"));
}

// Control: the positional and the in-order named spellings keep their value.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           substring_positional_and_in_order_named_unchanged_9_6) {
    std::string got = aotStdout(kSubstringControl);
    if (got != "bc\nbc")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("bc\nbc"));
}

// The reordered `Replace` must take the OLD operand from the `oldValue`
// PARAMETER and the NEW one from the `newValue` PARAMETER.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           replace_reordered_named_args_bind_by_parameter_name_22_5) {
    std::string got = aotStdout(kReplaceReordered);
    if (got != "aXcdef")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("aXcdef"));
}

// Control: the positional and the in-order named spellings keep their value.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           replace_positional_and_in_order_named_unchanged_9_6) {
    std::string got = aotStdout(kReplaceControl);
    if (got != "aXcdef\naXcdef")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("aXcdef\naXcdef"));
}

// The reordered `AsSpan` must take the START from the `start` PARAMETER and
// the COUNT from the `length` PARAMETER: the span's Length is 2.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           as_span_reordered_named_args_bind_by_parameter_name_22_5) {
    std::string got = aotStdout(kAsSpanReordered);
    if (got != "2")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("2"));
}

// Control: the positional and the in-order named spellings keep their value.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           as_span_positional_and_in_order_named_unchanged_9_6) {
    std::string got = aotStdout(kAsSpanControl);
    if (got != "2\n2")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_EQ(got, std::string("2\n2"));
}

// ===========================================================================
// The rows the front end rejects on this tree — the CODEGEN arm itself.
// ===========================================================================

// `CompareTo(other, ignoreCase)` is the OTHER `bool`-parameter member §9.6
// declares, and it is the same defect: read positionally, the reordered call
// hands the `i1` to the `ptr` `other` slot (malformed IR — the crash #7180
// reports for `Equals`). The case-folded ordinal comparison of "abc" and
// "ABC" is 0.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           compare_to_reordered_named_args_bind_by_parameter_name_22_5) {
    DirectRun r = runCodegenArm(kCompareToReordered);
    if (!r.built) std::fprintf(stderr, "  [stage] %s\n", r.stage.c_str());
    std::string got = r.built ? stripTrailingNewline(r.stdoutText) : std::string();
    if (got != "0")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(got, std::string("0"));
}

// Control: the positional and the in-order named spellings keep their value.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           compare_to_positional_and_in_order_named_unchanged_9_6) {
    DirectRun r = runCodegenArm(kCompareToControl);
    if (!r.built) std::fprintf(stderr, "  [stage] %s\n", r.stage.c_str());
    std::string got = r.built ? stripTrailingNewline(r.stdoutText) : std::string();
    if (got != "0\n0")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(got, std::string("0\n0"));
}

// `IndexOf(value, startIndex)`: the reordered call must take the NEEDLE from
// the `value` PARAMETER and the start from the `startIndex` PARAMETER — the
// code-point index of "c" in "abcdef" from index 1 is 2.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           index_of_reordered_named_args_bind_by_parameter_name_22_5) {
    DirectRun r = runCodegenArm(kIndexOfReordered);
    if (!r.built) std::fprintf(stderr, "  [stage] %s\n", r.stage.c_str());
    std::string got = r.built ? stripTrailingNewline(r.stdoutText) : std::string();
    if (got != "2")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(got, std::string("2"));
}

// Control: the positional and the in-order named spellings keep their value.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           index_of_positional_and_in_order_named_unchanged_9_6) {
    DirectRun r = runCodegenArm(kIndexOfControl);
    if (!r.built) std::fprintf(stderr, "  [stage] %s\n", r.stage.c_str());
    std::string got = r.built ? stripTrailingNewline(r.stdoutText) : std::string();
    if (got != "2\n2")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(got, std::string("2\n2"));
}

// `PadLeft(totalWidth, paddingChar)`: the reordered call must take the WIDTH
// from the `totalWidth` PARAMETER and the pad character from the
// `paddingChar` PARAMETER — "ab" padded to width 4 with 'x' is "xxab".
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           pad_left_reordered_named_args_bind_by_parameter_name_22_5) {
    DirectRun r = runCodegenArm(kPadLeftReordered);
    if (!r.built) std::fprintf(stderr, "  [stage] %s\n", r.stage.c_str());
    std::string got = r.built ? stripTrailingNewline(r.stdoutText) : std::string();
    if (got != "xxab")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(got, std::string("xxab"));
}

// Control: the positional and the in-order named spellings keep their value.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           pad_left_positional_and_in_order_named_unchanged_9_6) {
    DirectRun r = runCodegenArm(kPadLeftControl);
    if (!r.built) std::fprintf(stderr, "  [stage] %s\n", r.stage.c_str());
    std::string got = r.built ? stripTrailingNewline(r.stdoutText) : std::string();
    if (got != "xxab\nxxab")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(got, std::string("xxab\nxxab"));
}

// `Insert(startIndex, value)`: the reordered call must take the POSITION from
// the `startIndex` PARAMETER and the inserted text from the `value` PARAMETER
// — "X" at index 1 of "abcdef" is "aXbcdef".
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           insert_reordered_named_args_bind_by_parameter_name_22_5) {
    DirectRun r = runCodegenArm(kInsertReordered);
    if (!r.built) std::fprintf(stderr, "  [stage] %s\n", r.stage.c_str());
    std::string got = r.built ? stripTrailingNewline(r.stdoutText) : std::string();
    if (got != "aXbcdef")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(got, std::string("aXbcdef"));
}

// Control: the positional and the in-order named spellings keep their value.
BEZEL_TEST(codegen_string_named_arg_binding_7180,
           insert_positional_and_in_order_named_unchanged_9_6) {
    DirectRun r = runCodegenArm(kInsertControl);
    if (!r.built) std::fprintf(stderr, "  [stage] %s\n", r.stage.c_str());
    std::string got = r.built ? stripTrailingNewline(r.stdoutText) : std::string();
    if (got != "aXbcdef\naXbcdef")
        std::fprintf(stderr, "  [stdout] \"%s\"\n", got.c_str());
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_EQ(got, std::string("aXbcdef\naXbcdef"));
}
