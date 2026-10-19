// fix #2657 (§22.4 "Inline `out` declarations" / §22.3 "Parameter modifiers" /
// §2.5 "Dependency resolution"; v0.236) — an inline `out var` recipient of an
// IMPORTED METHOD was never given a type, producing a silent wrong value across
// `--import` and a SIGSEGV on one legal spelling of the read.
//
// (Revision note: the checkout these tests were written against carries v0.237
// in its header; the v0.236->v0.237 diff to docs/Bezel_Language_Spec.md is the
// version line only, so the §22.3 / §22.4 / §2.5 text quoted below is
// byte-identical at both revisions and the v0.236 citation the sibling
// #2629/#2639 commits use stays exact.)
//
// WHAT THE DEFECT WAS, and it is NOT the sibling #2639's cause. #2639 (the
// imported CONSTRUCTOR analogue) was an admission predicate that emitted no
// call at all. Here the call IS emitted — `BEZEL_DUMP_IR=1` on the issue's own
// repro shows `main` containing
//
//     %1 = alloca i32, align 4
//     %2 = call i1 @"bz$GLib.W4$Make"(ptr @.bz.str.49, ptr %1)
//
// i.e. the imported method is called correctly, but the caller's out-var slot
// is a 4-BYTE INTEGER ALLOCA standing in for an 8-byte `string` pointer. The
// callee then writes 8 bytes through it.
//
// The root cause is in SEMA, one level above the slot. `ExprTC::visitChildren`
// (src/sema/ExprTypecheck_Exprs.cpp, the CallExpr argument pre-pass) typed the
// inline `out var` local ONLY from the callee's `ast::MethodDecl`:
//
//     calleeMd2 = dynamic_cast<const ast::MethodDecl*>(ms2->decl);
//     ...
//     if (calleeMd2 && pos2 < calleeMd2->params.size() && ...)
//         lsym->declType = ctx_.resolvedType(*calleeMd2->params[pos2].type);
//
// An IMPORTED method has no `ast::MethodDecl` at all — `Loader::convertMember`
// (ModuleLoader.cpp:940 — `m->params = convertParams(mm.params, scope);`)
// reconstructs a `sema::MethodSymbol` with a null `decl`, and the formal
// parameter types survive only as `sema::ParameterSymbol::resolvedDeclType`
// (`Loader::convertParams`, ModuleLoader.cpp:558 —
// `p->resolvedDeclType = convert(mp.type);`). So `lsym->declType` stayed NULL,
// and two things
// followed:
//
//   * the recipient identifier never got an exprType either (the
//     `if (loc->declType)` guard in identifier resolution), so a later
//     `n.Length` failed to lower — the `internal:` #2491 / #2434 notes in the
//     issue — and the program silently returned 0; and
//   * codegen's `ensureOutVarSlots` (LlvmBackend_Calls2.cpp:1505) fell through
//     its ENTIRE `slotTy` cascade to the `i32()` DEFAULT, because its own
//     fallback is likewise `dynamic_cast<const ast::MethodDecl*>(ms->decl)`.
//
// §22.4 is explicit that this type exists and where it comes from: an inline
// `out var name` "declares a new variable inline. Its type is inferred from the
// method's `out` parameter declaration; an explicit type annotation is not
// permitted in the inline form." There is no spelling the user could have
// written to repair it. §22.3 then requires that "on normal return the slot's
// value is copied to the caller's recipient" — with a 4-byte slot for an
// 8-byte value that copy is a wild write.
//
// THE FIX has two halves.
//
//   (1) src/sema/ExprTypecheck_Exprs.cpp — `formalOutParamType2` resolves the
//       formal through the AST decl when there is one (so a LOCAL callee is
//       bit-identical) and falls back to the imported `ParameterSymbol`'s
//       `resolvedDeclType` otherwise. This is the load-bearing half: it is what
//       gives the recipient a type at all.
//   (2) src/codegen/LlvmBackend_Calls2.cpp — the same fallback in
//       `ensureOutVarSlots`' `slotTy` cascade. With (1) in place the earlier
//       `loc->declType` arm takes it, so (2) is a BELT — kept deliberately,
//       because a mis-sized out-var slot is a wild write by the callee, the
//       exact failure mode #2639 recorded as the reason its own two halves had
//       to ship together. It makes the caller's slot and the callee's own
//       signature derive from one field.
//
// WHY BOTH, measured rather than reasoned. Each half alone makes T1-T7 below
// return the right value, so neither is redundant for the *values*. But the
// codegen-only build leaves the recipient UNTYPED in sema, which is a
// type-checking hole, not a cosmetic one — mechanically confirmed on the plain
// path:
//
//     var q: int := n;            // n is `string`
//       codegen-only:  NOT rejected; dies in the backend with malformed IR
//                      ("Function return type does not match operand type")
//       both halves:   rejected, "the initializer for `q` has a type that is
//                      not implicitly convertible to the target type (§5.1)"
//
//     return n.NoSuchMemberAtAll; // no such member
//       codegen-only:  NOT rejected — COMPILES CLEAN and returns 0
//       both halves:   rejected with a real diagnostic
//
// The second row is a silent wrong answer for a program that must not compile,
// which is why T11 below pins it.
//
// ⚠ THE PLAIN BUILD IS THE ORACLE HERE, NOT ASAN. Recorded on #2629/#2639 and
// re-confirmed for this issue: the dependency artifact is built WITHOUT
// instrumentation (`--sanitize=address` is rejected together with `-c`), so the
// imported callee's over-wide write into the caller's under-sized slot is
// performed by uninstrumented code and ASan does not see it. Every row below
// was verified red-without / green-with on the plain path.
//
// MECHANICAL FAIL->PASS (plain path; `git checkout --` of both source files,
// rebuild, re-run, then `git apply` of the byte-identical patch):
//
//     case                                                  pre-fix   post-fix
//     T1  inline `out var` string, returned directly          0          5
//     T2  inline `out var` string, bound to a local        SIGSEGV       5
//     T3  inline `out var` long (value exceeds i32)           0         21
//     T4  inline `out var` bool                          COMPILE-FAIL   11
//     T5  inline `out var` string?                            0          4
//     T6  two out vars in one call, mixed widths         COMPILE-FAIL   35
//     T7  imported INSTANCE method                            0          6
//     T8  control: pre-declared `out` recipient               5          5
//     T9  control: `ref` int                                110        110
//     T10 control: no modifiers at all                       49         49
//     T11 must-reject: member that does not exist        ACCEPTED    rejected
//
// T4 and T6 are pre-fix COMPILE failures rather than wrong values: the i32 slot
// made the backend emit malformed IR (`br i32 %6` for the bool recipient), which
// `verifyModule` rejects. They are included because they show the defect is not
// confined to silently-wrong values.
//
// T8 is deliberately given an initializer whose LENGTH (7) differs from the
// value the callee writes (5) and from 0. #2639's own report was misled by
// exactly this: its control's untouched initializer `"zz"` had the same length
// as the expected `"hi"`, so a completely skipped call looked correct. The
// pre-declared form is genuinely unaffected here — its local has its own
// LocalDecl type — but the control is only worth having if a stale read cannot
// masquerade as a pass.
//
// NOT COVERED, filed separately: `out owned var` and `out using var` recipients
// of an imported method are broken the same way and at the same site
// (ExprTypecheck_Exprs.cpp, the sibling `if (calleeMd2 && ...)` gates for those
// two modifiers). They are left alone here because setting their declType also
// switches on ownership tracking that is currently dormant for them, which is a
// disposal-correctness change this fix cannot verify with a plain-path exit
// code. Named here so a later reader does not mistake their absence for
// coverage.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2657 {
    std::string path;
    explicit TempFile2657(std::string p) : path(std::move(p)) {}
    ~TempFile2657() { std::remove(path.c_str()); }
};

void writeSource2657(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library. Every out-parameter width under test lives here so a
// case pays for exactly one library compile. Values are chosen so that a slot
// that is never written (0), or written through a 4-byte alloca, cannot produce
// the expected answer: the `long` is larger than i32 can hold and every string
// length is distinct from 0 and from every other one.
const char* kLib2657 =
    "module OutMethLib2657; "
    "public class W5 "
    "  public v: int; "
    "  public static method MakeStr(s: string, out n: string): bool "
    "    n := \"abcde\"; return true; "
    "  end MakeStr; "
    "  public static method MakeLong(s: string, out n: long): bool "
    "    n := 5000000000L; return true; "
    "  end MakeLong; "
    "  public static method MakeBool(s: string, out n: bool): bool "
    "    n := true; return true; "
    "  end MakeBool; "
    "  public static method MakeNullable(s: string, out n: string?): bool "
    "    n := \"wxyz\"; return true; "
    "  end MakeNullable; "
    "  public static method MakeTwo(out a: string, out b: long): bool "
    "    a := \"pqrst\"; b := 9000000000L; return true; "
    "  end MakeTwo; "
    "  public static method Bump(ref n: int): bool "
    "    n := n + 10; return true; "
    "  end Bump; "
    "  public static method Plain(a: int): int "
    "    return a + 40; "
    "  end Plain; "
    "  public method Inst(s: string, out n: string): bool "
    "    n := \"ijklmn\"; return true; "
    "  end Inst; "
    "  public constructor() "
    "    v := 1; "
    "  end; "
    "end W5; "
    // §26.1: an imported EXTENSION method (T12). Built by a different
    // ModuleLoader path than the class's own methods, with the receiver on the
    // ExtensionDecl rather than in `params` — but `decl` is left null there
    // too, so the same blind spot is reached by another route.
    "public extension W5 "
    "  public method Fetch(out n: string): bool "
    "    n := \"mnopqr\"; return true; "
    "  end Fetch; "
    "end;";

// Build the dependency object, compile the app against it, run the app in a
// SEPARATE process (fork-isolated, per tests/README.md). Returns the child's
// exit code; -1 on a compile/link/run failure; -2 when the dependency itself
// could not be produced (no toolchain — the caller skips).
int runImported2657(const std::string& mainBody, const std::string& moduleTag) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2657_" + moduleTag + "_" + unique_stem("");

    TempFile2657 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2657 aObj((dir / (stem + "_a.o")).string());
    writeSource2657(aSrc.path, kLib2657);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    std::string appSrc =
        "module OutMethApp2657" + moduleTag + "; import OutMethLib2657; "
        "class C "
        "  public static method Main(): int " + mainBody +
        "  end Main; "
        "end C;";
    TempFile2657 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2657 bExe((dir / (stem + "_b")).string());
    writeSource2657(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Compile-only variant for T11's must-reject case. Returns 1 when the importer
// compiled clean, 0 when the front end reported at least one error, -2 when the
// dependency could not be produced. NOTE: unlike `runAot`'s coarse `rejects()`
// helper, this cannot confuse a rejection with a runtime crash — no program is
// ever executed on this path, so `success == false` can only come from a
// front-end diagnostic.
int compileImported2657(const std::string& mainBody,
                        const std::string& moduleTag,
                        std::string* errOut = nullptr) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2657c_" + moduleTag + "_" + unique_stem("");

    TempFile2657 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2657 aObj((dir / (stem + "_a.o")).string());
    writeSource2657(aSrc.path, kLib2657);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    std::string appSrc =
        "module OutMethApp2657" + moduleTag + "; import OutMethLib2657; "
        "class C "
        "  public static method Main(): int " + mainBody +
        "  end Main; "
        "end C;";
    TempFile2657 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2657 bExe((dir / (stem + "_b")).string());
    writeSource2657(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    // Capture the compile's stderr when asked: `CompileResult` carries only a
    // bool + a count, and the diagnostic TEXT is what makes a must-reject case
    // non-vacuous (an unrelated failure would satisfy the count alone).
    bezel::CompileResult res;
    if (errOut) {
        TempFile2657 bErr((dir / (stem + "_b.err")).string());
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
        res = driver.compileFile(bSrc.path, bExe.path, deps);
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        *errOut = ss.str();
    } else {
        res = driver.compileFile(bSrc.path, bExe.path, deps);
    }
    std::remove(bExe.path.c_str());
    return (res.success && res.errorCount == 0) ? 1 : 0;
}

}  // namespace

// ===========================================================================
// T1 — the issue's own repro. §22.4: an inline `out var name` "declares a new
// variable inline. Its type is inferred from the method's `out` parameter
// declaration". Pre-fix no type was inferred, the read of `n.Length` failed to
// lower (`internal:` #2491) and the program silently returned 0.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_method_inline_out_var_string_receives_value) {
    int rc = runImported2657(
        "    var e := W5.MakeStr(\"x\", out var n); "
        "    _ := e; "
        "    return n.Length; ", "T1");
    if (rc == -2) return;  // toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// T2 — the SAME call whose recipient is bound to a local first. This is the
// spelling that SIGSEGVed (exit 139): the imported callee wrote an 8-byte
// pointer through the caller's 4-byte `alloca i32`. Asserting the VALUE rather
// than "did not crash" keeps this row honest if the slot is ever resized
// without the recipient being typed.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_method_inline_out_var_bound_to_local_does_not_crash) {
    int rc = runImported2657(
        "    var e := W5.MakeStr(\"x\", out var n); "
        "    _ := e; "
        "    var L := n.Length; "
        "    return L; ", "T2");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// T3 — an 8-byte VALUE out parameter (`out n: long`). The comparison value is
// deliberately larger than i32 can represent, so the mis-defaulted 4-byte slot
// cannot produce it by truncation luck. This is the row the issue's own
// "is a primitive out-var accidentally right?" question resolves: `int` is
// (i32 is what the default happens to be), `long` is not.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_method_inline_out_var_long_receives_wide_value) {
    int rc = runImported2657(
        "    var e := W5.MakeLong(\"x\", out var n); "
        "    _ := e; "
        "    if n = 5000000000L then "
        "      return 21; "
        "    end if; "
        "    return 22; ", "T3");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 21);
}

// ===========================================================================
// T4 — `out n: bool`. Bool lowers to i1, so the i32 default made the backend
// emit `br i32 %6`, which `verifyModule` rejects: pre-fix this program did not
// compile at all. Included to pin that the defect's blast radius is not limited
// to silently-wrong values.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_method_inline_out_var_bool_lowers_and_runs) {
    int rc = runImported2657(
        "    var e := W5.MakeBool(\"x\", out var n); "
        "    _ := e; "
        "    if n then "
        "      return 11; "
        "    end if; "
        "    return 12; ", "T4");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 11);
}

// ===========================================================================
// T5 — a NULLABLE reference out parameter (`out n: string?`). §22.4 (#1597)
// requires the recipient to keep the parameter's declared nullability so
// §7.4's nullable-member-access rule still applies; this row checks that the
// newly-inferred type is the nullable one (the `<> null` test and the `!` are
// both required to compile) and that the value arrives.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_method_inline_out_var_nullable_string_receives_value) {
    int rc = runImported2657(
        "    var e := W5.MakeNullable(\"x\", out var n); "
        "    _ := e; "
        "    if n <> null then "
        "      return n!.Length; "
        "    end if; "
        "    return 99; ", "T5");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 4);
}

// ===========================================================================
// T6 — TWO inline out vars in one call, at different widths (`out a: string`,
// `out b: long`). Exercises the per-argument position index rather than a
// single-argument happy path: a fallback that read the wrong parameter would
// size one of the two slots from the other's type.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_method_two_inline_out_vars_are_typed_positionally) {
    int rc = runImported2657(
        "    var e := W5.MakeTwo(out var a, out var b); "
        "    _ := e; "
        "    if b = 9000000000L then "
        "      return a.Length + 30; "
        "    end if; "
        "    return 1; ", "T6");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 35);
}

// ===========================================================================
// T7 — an imported INSTANCE method (the receiver is an imported class), as
// opposed to T1-T6's static ones. The symbol lookup that produced the null
// `decl` is the same, but the callee's `MemberAccessExpr` target is a value
// rather than a type name, which is a separate resolution path.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_instance_method_inline_out_var_receives_value) {
    int rc = runImported2657(
        "    var w := new W5(); "
        "    var e := w.Inst(\"x\", out var n); "
        "    _ := e; "
        "    var r := n.Length; "
        "    w.Dispose(); "
        "    return r; ", "T7");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 6);
}

// ===========================================================================
// T8 — CONTROL: a PRE-DECLARED `out n` recipient of the same imported method.
// This form was already correct (the local carries its own LocalDecl type) and
// must stay correct. The initializer's length (7) differs from the callee's
// value (5) and from 0, so a skipped or truncated copy-out cannot pass here —
// the coincidence that misled #2639's own report.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_method_predeclared_out_recipient_still_correct) {
    int rc = runImported2657(
        "    var n: string := \"zzzzzzz\"; "
        "    var e := W5.MakeStr(\"x\", out n); "
        "    _ := e; "
        "    return n.Length; ", "T8");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// T9 — CONTROL: `ref` on an imported method. §22.3: "`ref` — pass by
// reference ... The variable must be initialized before the call." A `ref`
// argument is always a pre-declared local, so it never went through the
// inline-out-var typing path; this pins that the fix did not disturb it.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_method_ref_argument_still_written_back) {
    int rc = runImported2657(
        "    var n: int := 100; "
        "    var e := W5.Bump(ref n); "
        "    _ := e; "
        "    return n; ", "T9");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 110);
}

// ===========================================================================
// T10 — CONTROL: an imported method with no argument modifiers at all. The
// broadest guard against the fix disturbing ordinary cross-module calls.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_method_without_modifiers_still_correct) {
    int rc = runImported2657("    return W5.Plain(9); ", "T10");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 49);
}

// ===========================================================================
// T11 — MUST-REJECT. Because the recipient had no type at all, sema could not
// check ANY read of it: this program, which names a member `string` does not
// have, COMPILED CLEAN pre-fix and silently returned 0. That is a silent wrong
// answer for a program that must not compile, and it is the half of this defect
// that the codegen slot fix alone does not repair. Post-fix the recipient is
// typed `string` and the read is rejected by the front end.
//
// This asserts a COMPILE outcome, not a run: no process is executed, so a
// runtime crash cannot be mistaken for a rejection here.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_method_out_var_recipient_is_type_checked) {
    std::string err;
    int ok = compileImported2657(
        "    var e := W5.MakeStr(\"x\", out var n); "
        "    _ := e; "
        "    return n.NoSuchMemberAtAll; ", "T11", &err);
    if (ok == -2) return;
    BEZEL_EXPECT_EQ(ok, 0);  // 0 == front end reported an error
    // Asserting the error COUNT alone would pass on any unrelated compile
    // failure — the vacuity trap. Pin the actual member name so the rejection
    // has to be the one this fix causes.
    BEZEL_EXPECT_TRUE(err.find("NoSuchMemberAtAll") != std::string::npos);
}

// ===========================================================================
// T12 — an imported EXTENSION method's inline `out var` (§26.1). Not merely a
// bonus shape: an imported extension method's `sema::MethodSymbol` is built by
// a DIFFERENT ModuleLoader path (`Loader::convertMember`'s extension-member
// loop, ModuleLoader.cpp:6902 — `loader.convertParams(mm.params, scope)` —
// which ends in `ctx.registerImportedExtension` at :6910) than an
// ordinary imported method, and it too leaves `decl` null while filling
// `params` via `convertParams` — so it has
// the same blind spot, reached by another route. §26.1 puts the receiver on the
// `ExtensionDecl` rather than in the parameter list, so the argument positions
// line up with `params` exactly as for a static method and the same positional
// fallback applies.
//
// This row was NOT in the original submission and is added because a reviewer
// suspected the fix reached it. Measured rather than assumed — the full matrix
// on this program, by selectively disabling each half:
//
//     both halves disabled   SIGSEGV (exit 139)
//     sema half only         6
//     codegen half only      6
//     both halves (shipped)  6
//
// So the shape WAS broken (and broken in its dangerous form, a wild write, not
// merely a wrong value), and either half repairs it — the same relationship the
// header records for T1-T7 rather than a new one.
// ===========================================================================
BEZEL_TEST(xmod_method_out_var_2657,
           imported_extension_method_inline_out_var_receives_value) {
    int rc = runImported2657(
        "    var w := new W5(); "
        "    var e := w.Fetch(out var n); "
        "    _ := e; "
        "    var r := n.Length; "
        "    w.Dispose(); "
        "    return r; ", "T12");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 6);
}

}  // namespace bezel::module
