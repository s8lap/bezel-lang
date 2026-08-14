// Spec-derived regression test for issue #3136 — THE DECLARED POSITIONAL
// CONSTRUCTOR'S ARITY IS CHECKED (language spec §17.7 "Standard-library
// builtins have a closed constructor surface", v0.241; §31.4; stdlib §3.3).
//
// §17.7, verbatim, on the property this file covers:
//
//   "For a standard-library builtin type — any type the standard-library
//    document declares — the constructor surface is exactly what that document
//    declares — `new T(...)` resolves only against a declared constructor,
//    never against a compiler-supplied fallback."
//
// #2994 grew sema's arm 2 to reject the UNDECLARED PARAMETERLESS form
// `new T()` for the two record-shaped builtins (`ExceptionInfo` per §31.4
// and `ReadOnlyStringSpan` per stdlib §3.3), and #3098 grew codegen's
// `lowerNew` arm to CONSTRUCT the 3-argument declared form. This file pins
// the half neither of those covered: the declared form's ARITY must itself
// be checked, so a 2- or 4-argument list is rejected the same way.
//
// ── WHAT IT REPLACES (measured on b2e1bf3, the parent) ─────────────────────
//
// `checkBuiltinClosedConstructorSurface` (src/sema/ExprTypecheck_Exprs.cpp)
// gated its arm 2 on `ne.args.empty()` — the parameterless form only — so
// any non-zero wrong-arity call sailed past sema and reached codegen, whose
// `lowerNew` arm is gated on `ne.args.size() == 3` and falls through to
// `nullptr` otherwise. The null fell through `lowerLocalDecl`'s #2434
// zero-init fallback: a record of NULL strings, the #2922 class. Reading
// the result through a concatenation — `"[" + e.TypeName + "]"` — exited
// 139 (SIGSEGV) on the null `bz_string*`:
//
//   var e := new ExceptionInfo("AAA", "BBB");   -- 2 args; §31.4 declares 3
//   Console.WriteLine("[" + e.TypeName + "]");  -- exit 139 (SIGSEGV)
//
// The exact same class for `new ReadOnlyStringSpan("abcdef", 1)` — §3.3
// declares `(Source, Start, Length)`, so 2 args is also undeclared. The
// IDENTICALLY-SHAPED USER RECORD §17.7 names as the reference rejected both
// forms already, so the language was inconsistent with itself on the
// builtin half.
// ───────────────────────────────────────────────────────────────────────────
//
// ── THE FIX SHAPE ──────────────────────────────────────────────────────────
//
// `kPositionalCtorOnlyBuiltinRecords` (src/common/BuiltinRegistry.cpp)
// gained a parallel `kPositionalCtorOnlyBuiltinRecordArities` table
// carrying the declared arity per record (3 for both current entries, kept
// per-record so a future arm-2 record with a different arity is just a
// table row, not a call-site update), and the sema arm now reads that
// arity and rejects any `ne.args.size()` that does not match it with the
// same §17.7 wording #2994 introduced. The codegen arm #3098 grew is
// reached only by the spelling the doc declares.
// ───────────────────────────────────────────────────────────────────────────
//
// ── ANTI-OVER-REJECT CONTROLS ──────────────────────────────────────────────
//
// The fix must not re-open what #2994 closed (parameterless form stays
// rejected) nor what #3098 grew (declared 3-arg form keeps constructing).
// Both are re-asserted here so this file owns the full §17.7 arm-2 surface
// in one place. The identically-shaped user record §17.7 names as the
// reference is also asserted, both accepted (3-arg) and rejected (0-arg
// and 2-arg) — the latter with §22.5's "required parameter `A` is not
// supplied" wording, not §17.7's.
//
// ── ORACLE CHOICE ──────────────────────────────────────────────────────────
//
// The §17.7 REJECTION oracle is the diagnostic TEXT the file uses — the
// property §2994 closed was the text "is not a declared constructor of
// ... (§17.7)", so a wrong-arity list must emit a diagnostic carrying that
// same §17.7 wording (the #2935 lesson: a diagnostic that only says "no
// constructor" leaves the reader to find the spelling that does exist).
// The "wrong-arity" diagnostic here keeps the §17.7 anchor and adds the
// declared arity, so the reader sees BOTH the rule and the count.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string newLocal(const char* ctor) {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  var x := ") + ctor + "; "
        "  Console.WriteLine(\"constructed\"); "
        "  return 0; "
        "end Main; end C;";
}

bool hasError(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// §17.7 discriminator for arm 2 — the wording #2994 introduced for the
// parameterless form. The fix reuses it for the wrong-arity case so a
// reader sees the same rule either way.
bool rejectsPositionalOnlyArm(const std::string& program) {
    return hasError(program, "is not a declared constructor of") &&
           hasError(program, "\xC2\xA7""17.7");
}

}  // namespace

// ── THE DEFECT (the two wrong-arity cases the issue names) ─────────────────

BEZEL_TEST(builtin_record_positional_ctor_arity_3136,
           two_argument_exceptioninfo_is_rejected_spec_17_7_31_4) {
    // §31.4 declares `record ExceptionInfo(TypeName: string, Message: string,
    // StackTrace: string)` — exactly three positional fields. A two-argument
    // list resolves against NEITHER a declared constructor NOR a parameterless
    // form (which #2994 already rejected), so §17.7's "the constructor surface
    // is exactly what that document declares" rule rejects it here.
    //
    // Measured on b2e1bf3 (the parent): this compiled, and the subsequent
    // `Console.WriteLine("[" + e.TypeName + "]")` exited 139 (SIGSEGV) on the
    // null `bz_string*` field — the #2922-class silent wrong value the issue
    // names.
    const std::string program =
        "module M; class C public static method Main(): int "
        "  var e := new ExceptionInfo(\"AAA\", \"BBB\"); "
        "  Console.WriteLine(\"[\" + e.TypeName + \"]\"); "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(rejectsPositionalOnlyArm(program));
}

BEZEL_TEST(builtin_record_positional_ctor_arity_3136,
           two_argument_readonlystringspan_is_rejected_spec_17_7_stdlib_3_3) {
    // stdlib §3.3 declares `record ReadOnlyStringSpan(Source: string, Start:
    // int, Length: int)` — exactly three positional fields. Same case as the
    // §31.4 test directly above; both records share the declared arity of 3
    // because both are arm-2 entries of `kPositionalCtorOnlyBuiltinRecords`.
    const std::string program =
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"abcdef\", 1); "
        "  Console.WriteLine(s.Length); "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(rejectsPositionalOnlyArm(program));
}

BEZEL_TEST(builtin_record_positional_ctor_arity_3136,
           four_argument_exceptioninfo_is_rejected_spec_17_7_31_4) {
    // The OTHER wrong-arity direction — too many arguments. §31.4 declares
    // three positional fields; a fourth argument list also resolves against
    // neither a declared constructor nor a parameterless form. Same wording.
    const std::string program =
        "module M; class C public static method Main(): int "
        "  var e := new ExceptionInfo(\"T\", \"M\", \"S\", \"extra\"); "
        "  Console.WriteLine(e.TypeName); "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(rejectsPositionalOnlyArm(program));
}

BEZEL_TEST(builtin_record_positional_ctor_arity_3136,
           four_argument_readonlystringspan_is_rejected_spec_17_7_stdlib_3_3) {
    const std::string program =
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"abcdef\", 1, 3, 99); "
        "  Console.WriteLine(s.Length); "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(rejectsPositionalOnlyArm(program));
}

BEZEL_TEST(builtin_record_positional_ctor_arity_3136,
           one_argument_exceptioninfo_is_rejected_spec_17_7_31_4) {
    // One-argument list — exercises a DIFFERENT wrong arity (1, not 2 or 4)
    // to pin that the rejection is keyed on the arity mismatch, not on a
    // specific count. Same wording as the 2- and 4-arg cases.
    const std::string program =
        "module M; class C public static method Main(): int "
        "  var e := new ExceptionInfo(\"only\"); "
        "  Console.WriteLine(e.TypeName); "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(rejectsPositionalOnlyArm(program));
}

// ── THE DECLARED ARITY IS NAMED IN THE DIAGNOSTIC ──────────────────────────

BEZEL_TEST(builtin_record_positional_ctor_arity_3136,
           the_wrong_arity_diagnostic_names_the_declared_arity_spec_17_7) {
    // The diagnostic must tell the reader WHAT arity is expected, not only
    // that some arity is wrong — the #2935 lesson. The declared arity is "3"
    // for both current arm-2 entries, so the diagnostic carries it.
    const std::string program =
        "module M; class C public static method Main(): int "
        "  var e := new ExceptionInfo(\"a\", \"b\"); "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "3 positional fields"));
}

// ── ANTI-OVER-REJECT: the parameterless form #2994 closed stays closed ─────

BEZEL_TEST(builtin_record_positional_ctor_arity_3136,
           the_parameterless_form_is_still_rejected_spec_17_7) {
    // §17.7 arm 2's pre-existing rejection (the `#2994` half). Growing the
    // arm to cover wrong arities must not re-open this — the 0-arg list is
    // still undeclared for both types, and the parameterless wording is the
    // SAME wording the wrong-arity path now also uses.
    BEZEL_EXPECT_TRUE(rejectsPositionalOnlyArm(newLocal("new ExceptionInfo()")));
    BEZEL_EXPECT_TRUE(
        rejectsPositionalOnlyArm(newLocal("new ReadOnlyStringSpan()")));
}

// ── ANTI-OVER-REJECT: the 3-arg declared form #3098 grew keeps constructing ─

BEZEL_TEST(builtin_record_positional_ctor_arity_3136,
           the_three_argument_declared_form_keeps_constructing_spec_17_7) {
    // The other half of #2994's wording: §17.7 KEEPS the positional primary
    // constructor, and #3098 grew codegen's `lowerNew` arm to construct it.
    // A new arm that rejects wrong arities must NOT swallow the right one.
    // The oracle is the value the construction produces — both types'
    // declared fields round-trip through the `Console.WriteLine` path the
    // #2922-class SIGSEGV lives on.
    auto ei = runAot(
        "module M; class C public static method Main(): int "
        "  var e := new ExceptionInfo(\"TypeName\", \"Msg\", \"Stack\"); "
        "  Console.WriteLine(\"[\" + e.TypeName + \"]\"); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(ei.ran);
    BEZEL_EXPECT_EQ(0, ei.exitCode);
    BEZEL_EXPECT_TRUE(ei.stdoutText.find("[TypeName]") != std::string::npos);

    auto rs = runAot(
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"abcdef\", 1, 3); "
        "  Console.WriteLine(s.ToString()); "
        "  return s.Length; end Main; end C;");
    BEZEL_EXPECT_TRUE(rs.ran);
    BEZEL_EXPECT_EQ(3, rs.exitCode);
    BEZEL_EXPECT_TRUE(rs.stdoutText.find("bcd") != std::string::npos);
}

// ── REFERENCE: an identically-shaped USER record behaves the same way ──────

BEZEL_TEST(builtin_record_positional_ctor_arity_3136,
           a_user_record_also_rejects_wrong_arity_spec_22_5) {
    // §17.7's "exactly as for an identically-shaped user record" — the
    // reference behaviour the issue cites for §22.5's required-parameter
    // rule. The user twin's rejections carry §22.5's diagnostic, not
    // §17.7's — that's how the spec keeps the two halves separate: §17.7's
    // wording names the rule for builtins, §22.5's names the rule for
    // user-defined records.
    const std::string decls =
        "module M; record UR(A: string, B: string, C: string); ";
    // (a) too FEW: §22.5's wording, NOT §17.7's.
    const std::string tooFew =
        decls +
        "class C public static method Main(): int "
        "  var r := new UR(\"a\", \"b\"); "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(tooFew, "required parameter `C` is not supplied"));
    BEZEL_EXPECT_FALSE(rejectsPositionalOnlyArm(tooFew));
    // (b) too MANY: §22.5's wording, NOT §17.7's — bindArgsToParams emits
    // "supplies N argument(s) but the parameter list has M (§22.5)".
    const std::string tooMany =
        decls +
        "class C public static method Main(): int "
        "  var r := new UR(\"a\", \"b\", \"c\", \"d\"); "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(tooMany, "supplies 4 argument"));
    BEZEL_EXPECT_TRUE(hasError(tooMany, "parameter list has 3"));
    BEZEL_EXPECT_FALSE(rejectsPositionalOnlyArm(tooMany));
    // (c) the declared 3-arg form: still constructs.
    auto right = runAot(
        decls +
        "class C public static method Main(): int "
        "  var r := new UR(\"a\", \"bb\", \"ccc\"); "
        "  return r.A.Length + r.B.Length + r.C.Length; end Main; end C;");
    BEZEL_EXPECT_TRUE(right.ran);
    BEZEL_EXPECT_EQ(6, right.exitCode);                   // 1 + 2 + 3
}

// ── OTHER §17.7 BUILTINS UNTOUCHED ─────────────────────────────────────────

BEZEL_TEST(builtin_record_positional_ctor_arity_3136,
           the_no_constructor_builtins_keep_their_full_rejection_spec_17_7) {
    // §17.7 arm 1 — `Guid`/`IntPtr`/`UIntPtr` "admit no `new T(...)` at any
    // argument list". Untouched by this fix; re-asserted so a future edit
    // here cannot silently regress arm 1 while growing arm 2.
    for (const char* ctor : {"new Guid()", "new Guid(1)",
                             "new IntPtr()", "new IntPtr(0)",
                             "new UIntPtr()"}) {
        BEZEL_EXPECT_TRUE(hasError(newLocal(ctor),
                                   "is not admitted at any argument list"));
    }
}
