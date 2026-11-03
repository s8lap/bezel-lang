// The `required` (DECLARED MINIMUM ARITY) column of the ONE built-in
// parameter table (`common::kBuiltinParams`, src/common/BuiltinRegistry.cpp)
// — issue #8034.
//
// WHAT THE RATCHET SAID. The ratchet `scripts/check_builtin_declared_params.py`
// compared the table's parameter TYPE and §7.3 NULLABILITY columns with the
// standard library but never its `required` column — and when it started
// comparing it (commit 67531602, "the §-citations become data the checker
// verifies, and the `required` column is compared with the document"), it
// found EIGHT rows whose `required` column disagrees with the DOCUMENT'S
// OWN DECLARATION. The ratchet REPORTED them (the table is the subject of
// that ratchet, not its property); this file is the fix on the TABLE side.
//
// THE EIGHT ROWS, AND THE DOCUMENT'S OWN DECLARATION FOR EACH:
//
//   stdlib §7.2  constructor(path: string, mode: FileMode,
//                           access: FileAccess := FileAccess.ReadWrite)
//                 — `access` is DEFAULTED, so the declared MINIMUM is 2.
//   stdlib §2.2  method DivRem(a: int, b: int, out remainder: int): int
//                 — an `out` SLOT IS AN ARGUMENT (spec §22.3), so the
//                   declared MINIMUM is 3, not 2.
//   stdlib §7.4  method Write(value: string)      — MINIMUM 1, not 0.
//   stdlib §7.4  method WriteLine(value: string)   — MINIMUM 1, not 0.
//   stdlib §9.6  method IndexOf(value: char, startIndex: int := 0): int
//                 — `startIndex` is DEFAULTED, so the MINIMUM is 1, not 2.
//   stdlib §9.6  method PadCenter(totalWidth: int, paddingChar: char := ' ')
//   stdlib §9.6  method PadLeft(totalWidth: int, paddingChar: char := ' ')
//   stdlib §9.6  method PadRight(totalWidth: int, paddingChar: char := ' ')
//                 — `paddingChar` is DEFAULTED, so the MINIMUM is 1, not 2.
//
// WHY `required` IS NOT COSMETIC. The table's own contract (the header of
// src/common/BuiltinRegistry.cpp) says: "`required` counts the LEADING
// parameters that declare no default, so one row answers for every
// admissible argument count of an overload with §22.5 optional parameters",
// and the `File.ReadAllTextAsync` comment spells it out: "The `required`
// column is the declared MINIMUM, not the arity." The column therefore
// DRIVES TWO ADMISSION WINDOWS:
//
//   * `collectRows` (src/common/BuiltinRegistry.cpp) — the window
//     `required <= argCount <= total` that decides which declared form a
//     call at `argCount` arguments may bind against;
//   * `sema::FormalsView::ofNames(f.names, f.required)` — the §22.5
//     binder's `hasDefault(i) = i >= required_`, i.e. which trailing
//     parameter may be omitted.
//
// So changing the column changes WHICH CALLS COMPILE. This file pins the
// column's eight values AND the admission decisions they drive.
//
// MEASURED BEHAVIOUR (bezelc, this tree vs its parent fc637cdd; the
// programs are the ones under /var/tmp/bezel-scratch/r38-p64-required):
//
//   * `Math.DivRem(a := 17, b := 5)` — NEWLY REFUSED. Before the fix the
//     table's `required` said 2, so the `out remainder` slot looked
//     OPTIONAL and the binder admitted the two-argument call; the call
//     then reached codegen, matched no `DivRem` arm (which requires three
//     arguments), and degraded to a zero-initialised value with the #2434
//     warning "lowerLocalDecl's initializer expression failed to lower to
//     a value". After the fix the declared minimum is 3, so the two-
//     argument call binds no declared form and the §22.5 binder refuses
//     it with a real diagnostic.
//   * `Math.DivRem(17, 5, out r)` — STILL ADMITTED. The `out` slot IS an
//     argument, so the three-argument form is the declared one.
//   * `Math.DivRem(17, 5)` (POSITIONAL, two arguments) — UNCHANGED: still
//     admitted by the front end, still degraded at codegen. The front end
//     has NO arity gate on the `Math` head (`checkBuiltinMathConvertCall-
//     Args` stands down when no declared form has the call's arity, and
//     `checkBuiltinCallAdmission` does not cover `Math`), so the
//     POSITIONAL two-argument spelling is a pre-existing hole this fix
//     does not close. The row below pins that the fix does not make it
//     WORSE (no new refusal), and the report says so.
//   * `string.PadLeft` / `PadRight` / `PadCenter` / `IndexOf` at ONE
//     argument — UNCHANGED: already admitted before the fix, because the
//     table carries a SEPARATE one-argument row for each of them (the
//     §5.1 admission gate is keyed on the EXACT argument count). The fix
//     makes the TWO-argument row's minimum honest, and the visible effect
//     is on a NAMED call that binds no candidate: it now says "the named
//     arguments match none of the declared overloads at 1 argument(s) —
//     the standard library declares (totalWidth) | (totalWidth,
//     paddingChar)" instead of "has no parameter named `paddingChar`",
//     because the two-argument form now genuinely admits one argument.
//   * `new FileStream(path, mode)` — UNCHANGED: already admitted before
//     the fix (the table carries a two-argument row as well). The visible
//     effect is the same one: a NAMED call that binds no candidate now
//     names BOTH declared overloads, "(path, mode) | (path, mode,
//     access)", because the three-argument form now genuinely admits two
//     arguments.
//   * `TextWriter.Write` / `TextWriter.WriteLine` — UNCHANGED. Their rows
//     carry NO parameter NAMES (stdlib §7.4's note declares per-type
//     overloads, not a named parameter), so `collectRows` skips them and
//     the `required` column is read by NOTHING on the `TextWriter` head.
//     The column's correction is bookkeeping: §7.4's
//     `Write(value: string)` / `WriteLine(value: string)` declare a
//     minimum of ONE argument. The zero-argument `w.WriteLine()` is
//     stdlib §2.1's `Console.WriteLine()` — a DIFFERENT declaration — and
//     it keeps working (pinned below); the zero-argument `w.Write()` is
//     not on any surface and is refused (pinned below).
//
// FAIL-WITHOUT. Every table-level row, every admission-window row and the
// `Math.DivRem` refusal row FAIL on the parent tree (fc637cdd) and PASS on
// this one; the acceptance rows hold on BOTH, so they are the false-
// rejection guard.
//
// per spec §22.3, §22.5, §22.6, §5.1, §7.3; stdlib §2.1, §2.2, §7.2, §7.4,
// §9.6

#include <cstdio>
#include <string>
#include <vector>

#include "common/BuiltinRegistry.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::parseAndTypecheckGated;

namespace {

// ---------------------------------------------------------------------------
// The table-level rows.
// ---------------------------------------------------------------------------

// The `required` (DECLARED MINIMUM) column of the ONE declared form of
// `head.member` at `total` arguments. The table's `total` is the row's
// parameter count; `builtinParamForms` reads the rows by EXACT count.
std::size_t declaredMinimum(std::string_view head, std::string_view member,
                            std::size_t total) {
    const std::vector<bezel::common::BuiltinParamForm> forms =
        bezel::common::builtinParamForms(head, member, total);
    if (forms.empty()) return static_cast<std::size_t>(-1);
    // Every row of one (head, member, total) key carries the SAME declared
    // minimum — the document declares ONE form there — so the first row
    // speaks for the key. The Math.DivRem key has two rows; the second
    // row is asserted separately below.
    return forms.front().required;
}

// EVERY row of one (head, member, total) key, as its declared minimum.
// The `Math.DivRem` key carries TWO rows (the `int` and the `long`
// overload of stdlib §2.2), and BOTH must carry the declared minimum.
std::vector<std::size_t> declaredMinimums(std::string_view head,
                                          std::string_view member,
                                          std::size_t total) {
    std::vector<std::size_t> out;
    for (const auto& f : bezel::common::builtinParamForms(head, member, total))
        out.push_back(f.required);
    return out;
}

// The declared parameter names of a candidate the §22.5 binder may bind
// against, as "a, b, c".
std::string candidateNames(
    const std::vector<bezel::common::BuiltinFormals>& cands, std::size_t index) {
    if (index >= cands.size()) return "<none>";
    std::string s;
    for (std::size_t i = 0; i < cands[index].names.size(); ++i) {
        if (i) s += ", ";
        s += std::string(cands[index].names[i]);
    }
    return s;
}

// ---------------------------------------------------------------------------
// The front-end rows.
// ---------------------------------------------------------------------------

// A whole program whose `Main` body is `body`.
std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The same, plus the §7.2 `System.IO` enums SPELLED OUT. The real driver
// injects them as an auto-imported prelude (src/driver/Driver.cpp's
// `kIoPreludeSource`), and a user declaration of the same name SHADOWS the
// prelude's (#2047 follow-up 2) — so this program means the same thing in
// the front-end-only helper (which has no prelude) and in the real
// compiler. The declaration ORDER fixes the ordinals the FileStream
// runtime consumes, exactly as the driver's prelude does.
std::string progWithIo(const std::string& body) {
    return "module M; "
           "public enum FileMode: byte "
           "  CreateNew, Create, Open, OpenOrCreate, Truncate, Append; "
           "end FileMode; "
           "public enum FileAccess: byte "
           "  Read, Write, ReadWrite; "
           "end FileAccess; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// True iff SOME error diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(sub) != std::string::npos)
            return true;
    return false;
}

void dumpDiagnostics(const bezel::test::SemaResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

}  // namespace

// ── the eight rows: the DECLARED MINIMUM the document states ───────────────

BEZEL_TEST(builtin_required_column_8023,
           filestream_ctor_minimum_is_two_because_access_is_defaulted) {
    // stdlib §7.2: `access: FileAccess := FileAccess.ReadWrite` — DEFAULTED,
    // so the declared minimum is TWO, and the three-argument row carries it.
    BEZEL_EXPECT_EQ(declaredMinimum("FileStream", "", 3), 2u);
}

BEZEL_TEST(builtin_required_column_8023,
           math_divrem_minimum_is_three_because_an_out_slot_is_an_argument) {
    // stdlib §2.2: `DivRem(a: int, b: int, out remainder: int): int` — the
    // `out` slot IS an argument (spec §22.3), so the minimum is THREE.
    const std::vector<std::size_t> mins = declaredMinimums("Math", "DivRem", 3);
    BEZEL_EXPECT_EQ(mins.size(), 2u);
    for (std::size_t m : mins) BEZEL_EXPECT_EQ(m, 3u);
}

BEZEL_TEST(builtin_required_column_8023, textwriter_write_minimum_is_one) {
    // stdlib §7.4: `public abstract method Write(value: string);` — one
    // parameter, no default, so the declared minimum is ONE.
    BEZEL_EXPECT_EQ(declaredMinimum("TextWriter", "Write", 1), 1u);
}

BEZEL_TEST(builtin_required_column_8023, textwriter_writeline_minimum_is_one) {
    // stdlib §7.4: `public method WriteLine(value: string);` — one
    // parameter, no default, so the declared minimum is ONE.
    BEZEL_EXPECT_EQ(declaredMinimum("TextWriter", "WriteLine", 1), 1u);
}

BEZEL_TEST(builtin_required_column_8023,
           string_indexof_minimum_is_one_because_startindex_is_defaulted) {
    // stdlib §9.6: `IndexOf(value: char, startIndex: int := 0): int` —
    // `startIndex` is DEFAULTED, so the minimum is ONE.
    BEZEL_EXPECT_EQ(declaredMinimum("string", "IndexOf", 2), 1u);
}

BEZEL_TEST(builtin_required_column_8023,
           string_padcenter_minimum_is_one_because_paddingchar_is_defaulted) {
    BEZEL_EXPECT_EQ(declaredMinimum("string", "PadCenter", 2), 1u);
}

BEZEL_TEST(builtin_required_column_8023,
           string_padleft_minimum_is_one_because_paddingchar_is_defaulted) {
    BEZEL_EXPECT_EQ(declaredMinimum("string", "PadLeft", 2), 1u);
}

BEZEL_TEST(builtin_required_column_8023,
           string_padright_minimum_is_one_because_paddingchar_is_defaulted) {
    BEZEL_EXPECT_EQ(declaredMinimum("string", "PadRight", 2), 1u);
}

// ── the admission window the column drives ────────────────────────────────

BEZEL_TEST(builtin_required_column_8023,
           the_filestream_two_argument_form_is_a_declared_candidate) {
    // `collectRows`' window is `required <= argCount <= total`. With the
    // three-argument row's minimum corrected to TWO, the two-argument call
    // `new FileStream(path, mode)` may bind against the THREE-parameter
    // declaration `(path, mode, access)` with `access` taking its default.
    const auto cands =
        bezel::common::builtinCtorParamNameCandidates("FileStream", 2);
    BEZEL_EXPECT_EQ(cands.size(), 2u);
    BEZEL_EXPECT_EQ(candidateNames(cands, 0), std::string("path, mode"));
    BEZEL_EXPECT_EQ(candidateNames(cands, 1), std::string("path, mode, access"));
    BEZEL_EXPECT_EQ(cands[1].required, 2u);
}

BEZEL_TEST(builtin_required_column_8023,
           the_two_argument_math_divrem_call_binds_no_declared_form) {
    // The `out remainder` slot is an argument, so the two-argument call
    // binds no declared form — the §22.5 binder must refuse it.
    BEZEL_EXPECT_TRUE(
        bezel::common::builtinParamNameCandidates("Math", "DivRem", 2).empty());
    // The three-argument call still binds, with all three required.
    const auto cands = bezel::common::builtinParamNameCandidates("Math", "DivRem", 3);
    BEZEL_EXPECT_EQ(cands.size(), 2u);
    BEZEL_EXPECT_EQ(cands[0].required, 3u);
}

BEZEL_TEST(builtin_required_column_8023,
           the_one_argument_string_forms_bind_the_two_argument_declaration) {
    // stdlib §9.6's `paddingChar` / `startIndex` are DEFAULTED, so the
    // two-argument declaration also admits ONE argument — and the §22.5
    // binder may bind a one-argument call against it.
    for (const char* member : {"IndexOf", "PadLeft", "PadRight", "PadCenter"}) {
        const auto cands =
            bezel::common::builtinParamNameCandidates("string", member, 1);
        BEZEL_EXPECT_EQ(cands.size(), 2u);
        if (cands.size() != 2) continue;
        BEZEL_EXPECT_EQ(cands[0].required, 1u);
        BEZEL_EXPECT_EQ(cands[1].required, 1u);
    }
}

BEZEL_TEST(builtin_required_column_8023,
           the_textwriter_rows_carry_no_parameter_names) {
    // WHY the `TextWriter` rows' `required` column moves nothing: §7.4's
    // note declares per-TYPE `Write` / `WriteLine` overloads, not a NAMED
    // parameter, so the rows carry no names, `collectRows` skips them, and
    // the name table answers NOTHING for the `TextWriter` head at any
    // argument count. The `required` column of these two rows is therefore
    // read by no admission window — its correction is bookkeeping, and the
    // two compile-level rows below pin that the SURFACE is unchanged.
    BEZEL_EXPECT_TRUE(
        bezel::common::builtinParamNameCandidates("TextWriter", "Write", 1).empty());
    BEZEL_EXPECT_TRUE(bezel::common::builtinParamNameCandidates(
                          "TextWriter", "WriteLine", 1)
                          .empty());
    BEZEL_EXPECT_TRUE(
        bezel::common::builtinParamNameCandidates("TextWriter", "Write", 0).empty());
}

// ── the compile-level rows ────────────────────────────────────────────────

BEZEL_TEST(builtin_required_column_8023,
           math_divrem_with_two_named_arguments_is_refused) {
    // FAIL-WITHOUT: on the parent tree the table's `required` said 2, so
    // the `out remainder` slot looked optional, this call was ADMITTED and
    // degraded to a zero value at codegen (#2434). Now the declared
    // minimum is 3 and the §22.5 binder refuses it.
    const auto r =
        parseAndTypecheckGated(prog("    var q := Math.DivRem(a := 17, b := 5); "));
    if (!hasError(r, "named arguments are not supported on this built-in member"))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(
        hasError(r, "named arguments are not supported on this built-in member"));
}

BEZEL_TEST(builtin_required_column_8023, math_divrem_with_the_out_slot_is_admitted) {
    // The control: the `out` slot IS an argument, so the THREE-argument
    // form is the declared one and it must keep compiling.
    const auto r = parseAndTypecheckGated(prog(
        "    var rem: int := 0; "
        "    var q := Math.DivRem(17, 5, out rem); "));
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(builtin_required_column_8023,
           math_divrem_with_two_positional_arguments_is_refused) {
    // The POSITIONAL two-argument spelling binds no declared form:
    // stdlib §2.2 declares `DivRem` at THREE arguments — the `out
    // remainder` slot IS an argument, and an `out` slot carries no default
    // (§22.3, §22.5) — so the `Math`-head ARITY GATE refuses the call: it
    // supplies 2 argument(s) and no declared form has that count (§22.6).
    //
    // THE GATE IS #8036's, NOT #8034's. The #8034 lane DISCLOSED this hole
    // in its own commit message — "the POSITIONAL two-argument
    // `Math.DivRem(a, b)` is still admitted by the front end and still
    // degrades to a zero value at codegen, because there is no arity gate on
    // the `Math` head ... the regression test pins that this change does not
    // make the hole worse" — and wrote THIS ROW to pin that. #8036 then
    // closed the hole (in `8aaaebb5`, "fix #6810"), and the row's "not newly
    // refused" assertion became RED.
    //
    // FAIL-WITHOUT (MEASURED on `48353f2b`, the base the #8036 gate cites),
    // with this program's source: the call was ADMITTED and DEGRADED to a
    // zero value at codegen — the compiler's own "internal:
    // lowerLocalDecl's initializer expression failed to lower to a value for
    // this local's slot (degraded to a zero-initialized value) — #2434"
    // warning. There is NO arity message on that base at all: the `Math` head
    // had no arity gate.
    //
    // WHAT THIS ROW PINS: the ARITY GATE. The table's `required` column is
    // pinned by its own row —
    // `math_divrem_minimum_is_three_because_an_out_slot_is_an_argument` (the
    // table's `DivRem` form carries `required == 3`) — and the two-argument
    // form's binding by
    // `the_two_argument_math_divrem_call_binds_no_declared_form`. Neither was
    // touched.
    const auto r = parseAndTypecheckGated(prog("    var q := Math.DivRem(17, 5); "));
    if (!hasError(r, "no matching overload of `Math.DivRem`"))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, "no matching overload of `Math.DivRem`"));
    BEZEL_EXPECT_TRUE(
        hasError(r, "stdlib §2.2 declares `DivRem` at 3 argument(s)"));
}

BEZEL_TEST(builtin_required_column_8023, the_one_argument_string_forms_are_admitted) {
    // stdlib §9.6 declares `startIndex` / `paddingChar` with a DEFAULT, so
    // the one-argument forms are declared. They were admitted before the
    // fix too (the table carries a separate one-argument row); this row is
    // the false-rejection guard for the corrected minimum.
    const auto r = parseAndTypecheckGated(prog(
        "    var a := \"abc\".IndexOf('b'); "
        "    var b := \"abc\".PadLeft(5); "
        "    var c := \"abc\".PadRight(5); "
        "    var d := \"abc\".PadCenter(5); "));
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(builtin_required_column_8023,
           the_one_argument_string_forms_are_admitted_with_names) {
    const auto r = parseAndTypecheckGated(prog(
        "    var a := \"abc\".IndexOf(value := 'b'); "
        "    var b := \"abc\".PadLeft(totalWidth := 5); "
        "    var c := \"abc\".PadRight(totalWidth := 5); "
        "    var d := \"abc\".PadCenter(totalWidth := 5); "));
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(builtin_required_column_8023,
           a_named_string_call_that_binds_no_candidate_names_the_overloads) {
    // The visible effect of the corrected minimum on the `string` rows: the
    // two-argument declaration now admits ONE argument, so a one-argument
    // call that names the SECOND parameter binds no candidate, and the
    // shared §22.5 binder says so — naming BOTH declared forms — instead
    // of claiming the name is unknown.
    const auto r =
        parseAndTypecheckGated(prog("    var b := \"abc\".PadLeft(paddingChar := 'x'); "));
    if (!hasError(r, "the named arguments match none of the declared overloads"))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(
        hasError(r, "the named arguments match none of the declared overloads"));
}

BEZEL_TEST(builtin_required_column_8023, the_two_argument_filestream_constructor_is_admitted) {
    // stdlib §7.2 declares `access` with a DEFAULT, so the two-argument
    // form is declared. Positional AND named.
    const auto r = parseAndTypecheckGated(progWithIo(
        "    using var fs := new FileStream(\"/tmp/bz_req_8023.bin\", "  // 5230-allow: compile-level only — the program is type-checked, never run, so the path is never opened
        "                              FileMode.Create); "));
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());

    const auto r2 = parseAndTypecheckGated(progWithIo(
        "    using var fs := new FileStream(path := \"/tmp/bz_req_8023.bin\", "  // 5230-allow: compile-level only — the program is type-checked, never run, so the path is never opened
        "                              mode := FileMode.Create); "));
    if (r2.diagnostics.hasErrors()) dumpDiagnostics(r2);
    BEZEL_EXPECT_FALSE(r2.diagnostics.hasErrors());
}

BEZEL_TEST(builtin_required_column_8023,
           the_three_argument_filestream_constructor_is_admitted) {
    const auto r = parseAndTypecheckGated(progWithIo(
        "    using var fs := new FileStream(path := \"/tmp/bz_req_8023.bin\", "  // 5230-allow: compile-level only — the program is type-checked, never run, so the path is never opened
        "                              mode := FileMode.Create, "
        "                              access := FileAccess.Write); "));
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(builtin_required_column_8023,
           a_named_filestream_call_that_binds_no_candidate_names_the_overloads) {
    // The visible effect of the corrected minimum on the `FileStream` row:
    // the three-parameter declaration now admits TWO arguments, so the
    // two-argument call has TWO declared candidates and the shared §22.5
    // binder names both.
    const auto r = parseAndTypecheckGated(progWithIo(
        "    using var fs := new FileStream(path := \"/tmp/bz_req_8023.bin\", "  // 5230-allow: compile-level only — the program is type-checked, never run, so the path is never opened
        "                              bogus := FileMode.Create); "));
    if (!hasError(r, "the named arguments match none of the declared overloads"))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(
        hasError(r, "the named arguments match none of the declared overloads"));
}

// ── the TextWriter pair: the column is bookkeeping, the surface is not ────

BEZEL_TEST(builtin_required_column_8023,
           the_zero_argument_textwriter_writeline_is_still_admitted) {
    // stdlib §2.1 declares `Console.WriteLine()` — the ZERO-argument form —
    // and §7.4's `TextWriter.WriteLine(value: string)` is a DIFFERENT
    // declaration whose minimum is ONE. The corrected `required` column
    // must not refuse the zero-argument form. The program RUNS and writes
    // the empty line §2.1's form writes.
    const JitResult r = bezel::test::runAot(prog(
        "    var w: TextWriter := Console.Out; "
        "    w.WriteLine(); "));
    if (!r.ran) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.stdoutText, std::string("\n"));
}

BEZEL_TEST(builtin_required_column_8023, the_zero_argument_textwriter_write_is_refused) {
    // `TextWriter.Write` has no zero-argument form on any surface: §7.4
    // declares `Write(value: string)` and §2.1 declares `Console.Write`
    // with a value only. The refusal is codegen's #2457 backstop (the
    // `TextWriter` rows carry no parameter NAMES, so the front end has no
    // §22.5 surface to refuse it with), and it is unchanged by the
    // corrected `required` column.
    const JitResult r =
        bezel::test::runAot(prog("    var w: TextWriter := Console.Out; "
                                  "    w.Write(); "));
    if (!hasError(r, "produced no value")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, "produced no value"));
}
