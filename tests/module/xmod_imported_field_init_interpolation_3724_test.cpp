// fix #3724 (§18.5 / §22.8 / §2.8.2 / §7.9, v0.286) — an imported class's or
// record's instance field initialized by a STRING INTERPOLATION (`$"v{H.Get()}"`)
// read back as a NULL pointer parked in a NON-NULLABLE `string` slot, so
// `.Length` and string `+` on it SEGFAULTed deterministically (they emit no null
// guard, precisely because a non-nullable slot is assumed by construction never
// to hold null; `Console.WriteLine` and `=` have their own null guards and so
// merely printed an empty line / compared false).
//
// ⚠ THE ISSUE'S STATED MECHANISM IS REFUTED. #3724 hypothesised a defect in the
// LOAD-TIME reconstruction (`ModuleLoader.cpp`'s `parseAndAttachFieldInit`
// throwaway `freshCtx` + the `allExprSymbols`/`allResolvedTypes`/`allExprTypes`
// remap loops not carrying some `InterpolatedStringExpr::parts[i]` sub-structure).
// MEASURED on HEAD (153fc26) with a real two-module build and `BEZEL_DUMP_IR=1`:
// the importer emits NO field-initializer code AT ALL for the interpolated field
// — not a wrong value, not a null-returning `lowerExpr` (#1386's ICE never fires),
// and not even the #2860 construction-site reject. The wire carries NOTHING, so
// there is nothing for the reconstruction to fail at. The reconstruction machinery
// is in fact perfectly capable of replaying an interpolation: wrapping the SAME
// initializer in a binary `+` (`$"v{H.Get()}" + ""`) makes it reconstruct and
// evaluate to the exporter's declared `"v42"` on this same pre-fix build (T9).
//
// THE ACTUAL ROOT CAUSE is a ZERO-WIDTH SPAN stamped by the parser.
// `Parser_Exprs.cpp`'s `InterpolatedStringExpr` branch pre-stamps
// `span.begin = span.end = current_.location` before calling
// `splitInterpolation`, so that helper's own diagnostics (empty hole,
// brace-in-format-spec) have somewhere to point. But a non-zero
// `span.begin.line` also makes `parsePrimaryExpression`'s `stampSpan` wrapper —
// which stamps `[firstToken, tokenAfter)` only onto a node whose span is still
// unset (`span.begin.line == 0`) — a NO-OP. Every other primary expression
// therefore carries a real extent and this one alone carried
// `span.end.offset == span.begin.offset`.
//
// That is not cosmetic. Every SOURCE-SLICING consumer guards on
// `span.begin.offset < span.end.offset` and silently degrades when the guard
// fails. `ModuleMetadataBuilder.cpp`'s `fieldInitText` carriage
// (#2860/#2871/#3723, ~line 1115) is one of them, so an interpolated-string
// field initializer never put a single byte on the wire:
//   * `SemaContext::importedInstanceFieldInitText` was EMPTY on the importer
//     side, so `emitOwnFieldInit`'s (class, LlvmBackend_Setup.cpp) and
//     `buildRecordValue`'s (record, LlvmBackend_Exprs2.cpp) #2874 replay had
//     nothing to reconstruct, AND
//   * the #2860 construction-site diagnostic — the LOUD fallback that exists
//     exactly so a non-carriable initializer is named rather than silently
//     defaulted — is gated on that same text being present, so it could not
//     fire either.
// The field was left on the calloc-zero null watermark. §7.9 is the guarantee
// broken — "no non-nullable field is ever observed unset", and a non-nullable
// `string` is not defaultable, so the explicit initializer §7.9 requires here
// is the ONLY thing that can satisfy it; §2.8.2 is the clause broken ("An
// external instantiation behaves identically to a same-module instantiation of
// the same generic at the same arguments"); §22.8 step 2 ("Field initializers,
// in declaration order") is the step skipped.
//
// THE FIX is one line in `src/parser/Parser_Exprs.cpp`: close the span
// (`e->span.end = current_.location;`) after `advance()`, exactly the upper
// bound `stampSpan` would have supplied — repairing the AUTHORITY for "where
// does this expression end" rather than teaching each slicing consumer a second
// spelling of it (LANE_RULES §6: no parallel classification predicate). The
// same `span.begin.offset < span.end.offset` guard also fronts
// `canonicalClauseText` (contract-clause carriage, same file), whose fallback
// is the literal text "<expression>"; that consumer is not reachable for this
// node today (a `requires`/`ensures`/`invariant` clause is boolean-typed, so a
// bare interpolation cannot BE one), but it is the same guard over the same
// authority and the repair covers it rather than leaving a second consumer
// waiting for its own bug report.
//
// §6b — INITIALIZER-EXPRESSION-KIND DIFF (measured, both legs, imported
// generic AND non-generic, class AND record; `matrix.sh` in this lane's
// scratch). What the pre-fix carriage EXCLUDED vs what the post-fix carriage
// ADMITS:
//   kind                                     pre-fix          post-fix
//   int literal              `:= 7`          folds (valueText)  unchanged
//   string literal           `:= "plain"`    folds (valueText)  unchanged
//   call (int)               `:= H.Get()`    replays 42         unchanged
//   call (string)            `:= H.Str()`    replays "s42"      unchanged
//   `new`                    `:= new Pt(3,4)` replays (record)  unchanged
//   static member read       `:= H.Val`      replays 11         unchanged
//   conditional              `:= (if …)`     replays 1          unchanged
//   lambda call              `:= (lambda…)(6)` replays 7        unchanged
//   const string concat      `:= "v" + "w"`  replays "vw"       unchanged
//   interpolation-in-binary  `:= $"…" + ""`  replays "v42"      unchanged (T9)
//   ---- the five rows this fix CHANGES, all previously silent-null ----
//   interpolation            `:= $"v{H.Get()}"`      NULL → SEGV   "v42"
//   verbatim interpolation   `:= $@"v{H.Get()}"`     NULL → SEGV   "v42"
//   formatted hole           `:= $"v{H.Get():X}"`    NULL → SEGV   "v2A"
//   multi-hole               `:= $"a{H.Str()}b{H.Get()}c"` NULL  "as42b42c"
//   hole-free interpolation  `:= $"plain"`           NULL → SEGV   "plain"
// ⚠ Those five rows are measured on the three legs that need the WIRE: an
// imported GENERIC class (T1), a GENERIC record (T2), and a NON-GENERIC record
// (T4) — plus the static shape (T9). The fourth leg, a NON-GENERIC CLASS (T3),
// was ALREADY CORRECT before this fix and is included as a control precisely
// because it takes a different path: it replays through the exporter's own
// exported `bz$<Type>$init` (#328 FINDING [7]), where the initializer runs in
// the DECLARING module's compilation and never crosses as text at all. Its
// FAIL-WITHOUT is a PASS, which is the evidence that the defect is in the
// carriage and not in interpolation lowering.
// NOTHING is newly REJECTED and nothing newly silenced: the one shape that was
// already a LOUD §2.8.2 reject before this fix (`:= "v" + H.Str()` — a
// non-constant string `+`, whose reconstruction genuinely fails) is a loud
// reject after it too, unchanged. That shape and its sibling
// `:= H.Str() + H.Str()` (which reconstructs and then emits MALFORMED IR,
// `add ptr`) are both PRE-EXISTING, measured on BOTH sides of this commit, and
// filed as #3748 — they ARE a real gap in the #2874 reconstruction's
// expression-type carriage, i.e. the class of defect #3724 hypothesised and
// this file refutes for the interpolation shape.
//
// ⚠ THE FAMILY LANDS WHOLE (LANE_RULES §6b): the class consumer
// (`emitOwnFieldInit`) and the record consumer (`buildRecordValue`) are fixed
// by the SAME one-line writer-side repair and are pinned together here (T1/T3
// class, T2/T4 record), generic and non-generic, so the two paths cannot drift
// apart again on this shape. #3671's own T5 parity row is flipped in the same
// commit from "both legs are null" to "both legs read v42".
//
// FAIL-WITHOUT / PASS-WITH (measured; the parser hunk reverted to its parent
// commit in place, libbezel_core and the module test binary relinked, the same
// filters re-run, then restored and relinked):
//   filter 3724 -- 3 passed, 8 failed.  FAILING without the fix: T0 (the span
//     assertion itself), T1, T2, T4, T5, T6, T9, and #3671's flipped T5 (which
//     the substring also matches). PASSING without it: T3, T7, T8 — the three
//     rows deliberately written as controls over paths this fix must not move.
//   filter 3671 -- 7 passed, 1 failed; the one failure is T5, i.e. this
//     commit's flip is the ONLY behavioural change to that file.
// With the fix: 11/11 and 8/8 respectively, and the lit diagnostic track
// 2219/2219.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "ast/Node.hpp"
#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

void writeSource3724(const fs::path& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile the LIBRARY module to an object with embedded `.bezel.meta` (§2.4).
bool buildLib3724(const fs::path& dir, const std::string& stem,
                  const std::string& libSrc, std::string& objOut) {
    fs::path src = dir / (stem + "_a.bzl");
    objOut = (dir / (stem + "_a.o")).string();
    writeSource3724(src, libSrc);
    bezel::Driver driver;
    bool ok = driver.compileObject(src.string(), objOut).success;
    return ok && std::ifstream(objOut).good();
}

// Compile the IMPORTER against `aObj`, run it, and hand back BOTH the exit
// status shape and the program's stdout.
//
// ⚠ LANE_RULES §3: an AOT exit status truncates to 8 bits, so the observable
// this file cares about — the replayed STRING — travels on stdout, never in the
// exit code. `ran` is false for a compile/link failure OR any abnormal death
// (a signal on the plain build, ASan's own `_exit` under the sanitizer gate):
// that disjunction is what makes these rows oracle-INDEPENDENT (§R12-G), since
// every row asserts the CORRECT VALUE rather than a crash signature.
struct RunOutcome3724 {
    bool compiled = false;
    bool ran = false;       // exited normally (WIFEXITED)
    int exitCode = -1;
    std::string out;        // captured stdout
};

RunOutcome3724 compileImportAndRun3724(
        const fs::path& dir, const std::string& stem, const std::string& bSrcText,
        const std::string& aObj,
        const codegen::CodegenOptions& opts = codegen::CodegenOptions{},
        const std::string& envPrefix = "") {
    RunOutcome3724 r;
    fs::path bSrc = dir / (stem + "_b.bzl");
    fs::path bExe = dir / (stem + "_b");
    fs::path bOut = dir / (stem + "_b.out");
    writeSource3724(bSrc, bSrcText);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    r.compiled = driver.compileFile(bSrc.string(), bExe.string(), deps, opts)
                     .success;
    if (!r.compiled) return r;

    std::string cmd = envPrefix + bExe.string() + " > " + bOut.string() +
                      " 2>/dev/null";
    int status = std::system(cmd.c_str());
    r.ran = (status != -1) && WIFEXITED(status);
    if (r.ran) r.exitCode = WEXITSTATUS(status);
    std::ifstream in(bOut);
    std::ostringstream ss;
    ss << in.rdbuf();
    r.out = ss.str();
    return r;
}

// The library shared by T1-T7: the four import shapes the #2874 replay family
// reaches (generic/non-generic × class/record), each with the SAME interpolated
// field initializer, plus the verbatim / formatted / multi-hole / hole-free
// variants and the T9 binary-wrapped control.
const char* kLib3724 =
    "module Lib3724; "
    "public class H3724 "
    "  public static method Get(): int => 42; "
    "  public static method Str(): string => \"s42\"; "
    "end H3724; "
    "public class GC3724<T> "
    "  public v: T; "
    "  public bInterp: string := $\"v{H3724.Get()}\"; "
    "  public bVerb: string := $@\"v{H3724.Get()}\"; "
    "  public bFmt: string := $\"v{H3724.Get():X}\"; "
    "  public bMulti: string := $\"a{H3724.Str()}b{H3724.Get()}c\"; "
    "  public bHoleFree: string := $\"plain\"; "
    "  public bWrapped: string := $\"v{H3724.Get()}\" + \"\"; "
    "  public constructor(v0: T) v := v0; end constructor; "
    "end GC3724; "
    "public record GR3724<T>(a: T) "
    "  public bInterp: string := $\"v{H3724.Get()}\"; "
    "  public bVerb: string := $@\"v{H3724.Get()}\"; "
    "  public bFmt: string := $\"v{H3724.Get():X}\"; "
    "  public bMulti: string := $\"a{H3724.Str()}b{H3724.Get()}c\"; "
    "  public bHoleFree: string := $\"plain\"; "
    "  public bWrapped: string := $\"v{H3724.Get()}\" + \"\"; "
    "end GR3724; "
    "public class NC3724 "
    "  public bInterp: string := $\"v{H3724.Get()}\"; "
    "end NC3724; "
    "public record NR3724(a: int) "
    "  public bInterp: string := $\"v{H3724.Get()}\"; "
    "end NR3724;";

std::string appSrc3724(const std::string& moduleName, const std::string& body) {
    return "module " + moduleName + ";\n"
           "import Lib3724;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// T0 — THE ROOT CAUSE, pinned WITHOUT a runtime: after parsing
// `:= $"v{H.Get()}"`, the `ast::InterpolatedStringExpr` must carry a
// NON-DEGENERATE span whose source slice is exactly the interpolated-string
// lexeme. Pre-fix `span.end.offset == span.begin.offset`, which is what makes
// every `span.begin.offset < span.end.offset`-guarded slicing consumer
// (ModuleMetadataBuilder's `fieldInitText`, `canonicalClauseText`) silently
// carry nothing. This row is the tightest possible statement of the defect —
// it fails on the parse alone, with no module boundary and no codegen.
// (§18.5 interpolated strings; issue #135 is the span-stamping contract this
// node was the one primary to escape.)
// ===========================================================================
BEZEL_TEST(xmod_imported_field_init_interpolation_3724,
           t0_interpolated_string_expr_carries_a_non_degenerate_span_18_5) {
    const std::string src =
        "module SpanProbe3724;\n"
        "public class H\n"
        "  public static method Get(): int => 42;\n"
        "end H;\n"
        "public class K\n"
        "  public b: string := $\"v{H.Get()}\";\n"
        "end K;\n";

    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(unit != nullptr);
    BEZEL_EXPECT_TRUE(!diags.hasErrors());

    // Locate the field's initializer without walking the whole AST: the only
    // InterpolatedStringExpr in this unit is the one under test.
    const ast::InterpolatedStringExpr* interp = nullptr;
    if (unit) {
        for (const auto& decl : unit->topDecls) {
            auto* td = dynamic_cast<const ast::TypeDecl*>(decl.get());
            if (!td || td->name != "K") continue;
            for (const auto& m : td->members) {
                auto* fd = dynamic_cast<const ast::FieldDecl*>(m.get());
                if (!fd || !fd->initializer) continue;
                interp = dynamic_cast<const ast::InterpolatedStringExpr*>(
                    fd->initializer.get());
            }
        }
    }
    BEZEL_EXPECT_TRUE(interp != nullptr);
    if (!interp) return;

    // The defect, stated directly.
    BEZEL_EXPECT_TRUE(interp->span.begin.line != 0);
    BEZEL_EXPECT_TRUE(interp->span.begin.offset < interp->span.end.offset);
    BEZEL_EXPECT_TRUE(interp->span.end.offset <= src.size());
    if (interp->span.begin.offset >= interp->span.end.offset ||
        interp->span.end.offset > src.size())
        return;
    // …and the consequence the metadata carriage depends on: slicing the source
    // by that span recovers the initializer text verbatim.
    std::string slice = src.substr(
        interp->span.begin.offset,
        interp->span.end.offset - interp->span.begin.offset);
    BEZEL_EXPECT_TRUE(slice.find("$\"v{H.Get()}\"") != std::string::npos);
}

// ===========================================================================
// T1 — GENERIC CLASS (`emitOwnFieldInit`'s #2874 replay arm, the class
// consumer). Pre-fix `g.bInterp` was a null pointer in a non-nullable `string`
// slot: `.Length` and `+` SEGFAULTed. Post-fix it is the exporter's declared
// "v42". Both crashing operations are exercised.
// ===========================================================================
BEZEL_TEST(xmod_imported_field_init_interpolation_3724,
           t1_generic_class_interpolated_field_replays_22_8_2_8_2) {
    const auto& dir = wtScratchDir("bz3724");
    std::string stem = unique_stem("bz3724_t1_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3724(dir, stem, kLib3724, aObj));

    auto r = compileImportAndRun3724(dir, stem,
        appSrc3724("App3724T1",
                   "    var g := new GC3724<int>(9);\n"
                   "    Console.WriteLine(\"[\" + g.bInterp + \"]\");\n"
                   "    Console.WriteLine(g.bInterp.Length.ToString());\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[v42]\n3\n"));
}

// ===========================================================================
// T2 — GENERIC RECORD (`buildRecordValue`'s #2874 replay arm, #3671's
// consumer). The record twin of T1: the family lands whole (LANE_RULES §6b).
// ===========================================================================
BEZEL_TEST(xmod_imported_field_init_interpolation_3724,
           t2_generic_record_interpolated_field_replays_9_1_22_8) {
    const auto& dir = wtScratchDir("bz3724");
    std::string stem = unique_stem("bz3724_t2_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3724(dir, stem, kLib3724, aObj));

    auto r = compileImportAndRun3724(dir, stem,
        appSrc3724("App3724T2",
                   "    var g := new GR3724<int>(9);\n"
                   "    Console.WriteLine(\"[\" + g.bInterp + \"]\");\n"
                   "    Console.WriteLine(g.bInterp.Length.ToString());\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[v42]\n3\n"));
}

// ===========================================================================
// T3 — NON-GENERIC CLASS. A non-generic class replays its instance field
// initializers through the exported `bz$<Type>$init` (#328 FINDING [7]), so
// this leg's value comes from the EXPORTER's own compilation rather than the
// importer's replay — a second, independent path over the same source shape,
// pinned so the two cannot disagree.
// ===========================================================================
BEZEL_TEST(xmod_imported_field_init_interpolation_3724,
           t3_non_generic_class_interpolated_field_22_8) {
    const auto& dir = wtScratchDir("bz3724");
    std::string stem = unique_stem("bz3724_t3_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3724(dir, stem, kLib3724, aObj));

    auto r = compileImportAndRun3724(dir, stem,
        appSrc3724("App3724T3",
                   "    var g := new NC3724();\n"
                   "    Console.WriteLine(\"[\" + g.bInterp + \"]\");\n"
                   "    Console.WriteLine(g.bInterp.Length.ToString());\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[v42]\n3\n"));
}

// ===========================================================================
// T4 — NON-GENERIC RECORD (`buildRecordValue` again, reached via #3723's
// writer-side widening). §9.1/§9.2 record construction is lowered inline by
// value at the `new` site, so there is no `bz$<Type>$init` for this shape and
// the field initializer MUST come off the wire — the same carriage T1/T2 need.
// ===========================================================================
BEZEL_TEST(xmod_imported_field_init_interpolation_3724,
           t4_non_generic_record_interpolated_field_replays_9_1_3723) {
    const auto& dir = wtScratchDir("bz3724");
    std::string stem = unique_stem("bz3724_t4_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3724(dir, stem, kLib3724, aObj));

    auto r = compileImportAndRun3724(dir, stem,
        appSrc3724("App3724T4",
                   "    var g := new NR3724(7);\n"
                   "    Console.WriteLine(\"[\" + g.bInterp + \"]\");\n"
                   "    Console.WriteLine(g.bInterp.Length.ToString());\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[v42]\n3\n"));
}

// ===========================================================================
// T5 — the remaining §18.5 interpolation SHAPES, on both a class and a record:
// a VERBATIM interpolated string (`$@"…"`, §18.7), a HOLE-FREE one (`$"plain"`
// — no expression parts at all, so it is not the holes that were broken but the
// node), a FORMATTED hole (`{…:X}`, §18.5 format spec → "v2A"), and a MULTI-part
// one. All four were the same silent null before the fix.
// ===========================================================================
BEZEL_TEST(xmod_imported_field_init_interpolation_3724,
           t5_verbatim_holefree_formatted_and_multipart_shapes_18_5_18_7) {
    const auto& dir = wtScratchDir("bz3724");
    std::string stem = unique_stem("bz3724_t5_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3724(dir, stem, kLib3724, aObj));

    auto r = compileImportAndRun3724(dir, stem,
        appSrc3724("App3724T5",
                   "    var c := new GC3724<int>(9);\n"
                   "    var d := new GR3724<int>(9);\n"
                   "    Console.WriteLine(\"[\" + c.bVerb + c.bHoleFree + "
                   "c.bFmt + c.bMulti + \"]\");\n"
                   "    Console.WriteLine(\"[\" + d.bVerb + d.bHoleFree + "
                   "d.bFmt + d.bMulti + \"]\");\n"
                   "    Console.WriteLine((c.bVerb.Length + c.bHoleFree.Length "
                   "+ c.bFmt.Length + c.bMulti.Length).ToString());\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // "v42" + "plain" + "v2A" + "as42b42c" = 3 + 5 + 3 + 8 = 19 characters.
    BEZEL_EXPECT_EQ(r.out, std::string("[v42plainv2Aas42b42c]\n"
                                       "[v42plainv2Aas42b42c]\n"
                                       "19\n"));
}

// ===========================================================================
// T6 — DISPOSAL EXACTLY ONCE, under the memory net, LOOP-AMPLIFIED to 200
// iterations per shape (LANE_RULES §3 / §R12-G: a one-shot leak row passes on
// both sides because the allocation is still reachable at exit).
//
// This is the memory-safety half of the fix (§6c): the replayed interpolation
// MINTS a fresh +1 `bz_string` per construction (`emitStringConcat`'s malloc'd
// rc=1 result), so giving a previously-inert slot a real value makes every
// consumer of that slot newly reachable — the field's release must now actually
// run, exactly once, on all four shapes. 800 constructions with generated-code
// AddressSanitizer + LeakSanitizer armed (`exitcode=99` distinguishes a
// sanitizer report from the program's own status) prove both directions: a
// missing release shows up as a leak, a double release as a heap-corruption
// report. The value is checked too, so a "clean because nothing ran" outcome
// (§6c's loud→quiet trade) cannot pass this row.
//
// The library object is deliberately NOT sanitizer-instrumented (`-c` builds
// reject `--sanitize=address` by design); ASan's allocator interceptors are
// process-wide once the executable links against them, so an allocation made in
// the un-instrumented library and freed in the instrumented importer — and vice
// versa — is still tracked.
// ===========================================================================
BEZEL_TEST(xmod_imported_field_init_interpolation_3724,
           t6_replayed_interpolation_disposes_exactly_once_asan_200x_28_2) {
    const auto& dir = wtScratchDir("bz3724");
    std::string stem = unique_stem("bz3724_t6_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3724(dir, stem, kLib3724, aObj));

    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;

    auto r = compileImportAndRun3724(dir, stem,
        appSrc3724("App3724T6",
                   "    var total := 0;\n"
                   "    for var i in 0..<200 do\n"
                   "      var a := new GC3724<int>(i);\n"
                   "      total := total + a.bInterp.Length;\n"
                   "      var b := new GR3724<int>(i);\n"
                   "      total := total + b.bInterp.Length;\n"
                   "      var c := new NC3724();\n"
                   "      total := total + c.bInterp.Length;\n"
                   "      var d := new NR3724(i);\n"
                   "      total := total + d.bInterp.Length;\n"
                   "    end for;\n"
                   "    Console.WriteLine(total.ToString());\n"
                   "    return 0;\n"),
        aObj, opts,
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0 ");
    BEZEL_EXPECT_TRUE(r.compiled);
    // A sanitizer report exits 99; an untrapped crash makes `ran` false. Both
    // are failures, and so is a clean exit that printed the wrong total.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 iterations x 4 shapes x len("v42") == 2400.
    BEZEL_EXPECT_EQ(r.out, std::string("2400\n"));
}

// ===========================================================================
// T7 — CONTROL, and the evidence that REFUTES #3724's stated mechanism: the
// SAME interpolation wrapped in a binary `+` (`$"v{H.Get()}" + ""`). The
// enclosing `BinaryExpr` gets its span from `parsePrimaryExpression`'s
// `stampSpan` wrapper as normal, so the wire carried this initializer's text —
// and the load-time reconstruction replayed the interpolation inside it to the
// correct "v42" — on the PRE-FIX build. That is what proves the reconstruction
// machinery (`freshCtx` + the symbol/type remap loops the issue suspected) was
// never the defect, and that this row must be UNCHANGED by the fix.
// ===========================================================================
BEZEL_TEST(xmod_imported_field_init_interpolation_3724,
           t7_interpolation_wrapped_in_binary_plus_unchanged_control) {
    const auto& dir = wtScratchDir("bz3724");
    std::string stem = unique_stem("bz3724_t7_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3724(dir, stem, kLib3724, aObj));

    auto r = compileImportAndRun3724(dir, stem,
        appSrc3724("App3724T7",
                   "    var c := new GC3724<int>(9);\n"
                   "    var d := new GR3724<int>(9);\n"
                   "    Console.WriteLine(\"[\" + c.bWrapped + d.bWrapped + "
                   "\"]\");\n"
                   "    return 0;\n"),
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[v42v42]\n"));
}

// ===========================================================================
// T8 — CONTROL: a fully LOCAL (same-module) interpolated field initializer, on
// a class and a record. This path never consults the wire at all (the field's
// `decl` is non-null, so neither replay arm is reached), so it was CORRECT
// before the fix and must be byte-for-byte unaffected by it — the guard against
// a span change leaking into same-module lowering.
// ===========================================================================
BEZEL_TEST(xmod_imported_field_init_interpolation_3724,
           t8_local_interpolated_field_initializer_unaffected_control) {
    const auto& dir = wtScratchDir("bz3724");
    std::string stem = unique_stem("bz3724_t8_");
    fs::path src = dir / (stem + ".bzl");
    fs::path exe = dir / stem;
    fs::path out = dir / (stem + ".out");
    writeSource3724(src,
                    "module LocalCtl3724;\n"
                    "public class HL\n"
                    "  public static method Get(): int => 42;\n"
                    "end HL;\n"
                    "public class LC\n"
                    "  public b: string := $\"v{HL.Get()}\";\n"
                    "end LC;\n"
                    "public record LR(a: int)\n"
                    "  public b: string := $\"v{HL.Get()}\";\n"
                    "end LR;\n"
                    "public class C3724T8\n"
                    "  public static method Main(): int\n"
                    "    var c := new LC();\n"
                    "    var d := new LR(1);\n"
                    "    Console.WriteLine(\"[\" + c.b + d.b + \"]\");\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C3724T8;\n");
    bezel::Driver driver;
    BEZEL_EXPECT_TRUE(driver.compileFile(src.string(), exe.string(), {}).success);
    std::string cmd = exe.string() + " > " + out.string() + " 2>/dev/null";
    int status = std::system(cmd.c_str());
    BEZEL_EXPECT_TRUE(status != -1 && WIFEXITED(status));
    if (status != -1 && WIFEXITED(status)) BEZEL_EXPECT_EQ(WEXITSTATUS(status), 0);
    std::ifstream in(out);
    std::ostringstream ss;
    ss << in.rdbuf();
    BEZEL_EXPECT_EQ(ss.str(), std::string("[v42v42]\n"));
}

// ===========================================================================
// T9 — the STATIC interpolated field initializer, a path this fix makes NEWLY
// REACHABLE (LANE_RULES §7: "giving a previously-inert slot a real value makes
// every consumer newly reachable").
//
// `ModuleMetadataBuilder`'s gate carries `fieldInitText` for a GENERIC type's
// field REGARDLESS of `static` (#2860/#2871: each instantiation needs its own
// replayed per-instantiation #1513 cell), and `ModuleLoader.cpp`'s attach loop
// is gated on `mm.fieldInitText.empty()`. So before this fix an interpolated
// STATIC field of an imported generic was skipped entirely — same silent null.
// After it, that field reaches `parseAndAttachFieldInit(isStatic=true)`, whose
// failure arm is a LOAD-TIME §2.8.2 REJECT rather than the instance path's
// silent no-op. This row pins the outcome that actually occurs: the
// reconstruction SUCCEEDS and the static reads the exporter's declared value on
// both a generic and a non-generic class. (Were it ever to start rejecting, the
// failure here is loud and located, which is the §2.8.2/§6c-correct direction —
// but it must not, and this row is what says so.)
// ===========================================================================
BEZEL_TEST(xmod_imported_field_init_interpolation_3724,
           t9_static_interpolated_field_initializer_replays_22_12_2_8_2) {
    const auto& dir = wtScratchDir("bz3724");
    std::string stem = unique_stem("bz3724_t9_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3724(dir, stem,
        "module Lib3724Static; "
        "public class HS3724 "
        "  public static method Get(): int => 42; "
        "end HS3724; "
        "public class GS3724<T> "
        "  public v: T; "
        "  public static readonly S: string := $\"s{HS3724.Get()}\"; "
        "  public constructor(v0: T) v := v0; end constructor; "
        "end GS3724; "
        "public class NS3724 "
        "  public static readonly S: string := $\"s{HS3724.Get()}\"; "
        "end NS3724;",
        aObj));

    auto r = compileImportAndRun3724(dir, stem,
        "module App3724T9;\n"
        "import Lib3724Static;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    var g := new GS3724<int>(1);\n"
        "    Console.WriteLine(\"[\" + GS3724<int>.S + NS3724.S + \"]\");\n"
        "    Console.WriteLine((GS3724<int>.S.Length + NS3724.S.Length)"
        ".ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        aObj);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("[s42s42]\n6\n"));
}

}  // namespace bezel::module
