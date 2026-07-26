// Spec-derived tests for issue #2164 — local null-narrowing state hygiene
// at alternative-path boundaries (§7.4, v0.214).
//
// §7.4 (§22.8's single-assignment rule) and §19.2 / §20 / §31.2 require
// that a narrowing fact established inside one ALTERNATIVE-PATH body
// (the then-arm of an `if`, a `case` arm, the body of a `try`) not be
// visible to the next alternative-path analysis (the next arm runs only
// when this one did NOT — so the establishing guard may not have run;
// the fact is unsound). The legacy `nullNarrowed_` set skipped this
// hygiene at three alternative-path boundaries: the elif CONDITION, the
// case-arm body, and the try catch-arm entry. Each is a compile-ACCEPT
// of a null deref that SIGSEGV'd at runtime — three probe-confirmed
// repros in the issue.
//
// The new `fieldNarrowed_` set (#2116 redo) handles all three boundaries
// correctly; the fix here is to give the LOCAL set the same
// state-hygiene treatment (the redo's ExprTypecheck.cpp comments mark
// each site).
//
// All three repros are PURE SEMANTIC — the compiler must REJECT the
// bad code at sema, never reach codegen, never SIGSEGV.

#include <cstdio>

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// -------------------------------------------------------------------------
// (1) elif CONDITION visited under the preceding branch BODY's exit state
// (`probe_elif_stale.bzl`, SIGSEGV; the issue's first repro). The IfStmt
// walk reset `nullNarrowed_` to the entry state only AFTER
// `visitExpr(*el.condition)`, so the member-access gate inside the elif
// condition judged against the then-body's leftovers. The fix resets to
// entry + preceding conditions' false-branch facts BEFORE the visit (the
// same discipline the field set applies — collectFieldNarrowings).
// -------------------------------------------------------------------------
BEZEL_TEST(local_null_narrow_alt_path_2164,
           elif_condition_then_body_exit_state_rejected_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method P(x: string?): int "
        "    if x <> null then Console.WriteLine(\"got it\"); return 100; "
        "    elif x.Length > 0 then return 1; end; "
        "    return -1; "
        "  end P; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

BEZEL_TEST(local_null_narrow_alt_path_2164,
           elif_condition_sound_accept_null_guard_then_elif_length_spec_7_4) {
    // The ACCEPT control: `if x = null then return 0; if x.Length > 0 then
    // return 1; end; end;` — the prior `if x = null` else-narrowing
    // establishes x non-null for the next statement. The local-set reset
    // before each statement's condition visit must restore the entry +
    // prior-false-branch facts (x non-null) so the sound ACCEPT is
    // preserved. The issue explicitly cites this as the sound converse
    // of the elif-staleness reject (#2164).
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method P(x: string?): int "
        "    if x = null then return 0; end; "
        "    if x.Length > 0 then return 1; end; "
        "    return -1; "
        "  end P; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// -------------------------------------------------------------------------
// (2) case-arm bodies share state arm-to-arm
// (`probe_case_arm_leak.bzl`, SIGSEGV). Neither CaseStmt nor CaseExpr
// reset `nullNarrowed_` per arm; arms are alternatives — each arm should
// start from case-entry state; after-case = entry ∩ arm exits. The fix
// applies the same running-intersection discipline the field set uses.
// -------------------------------------------------------------------------
BEZEL_TEST(local_null_narrow_alt_path_2164,
           case_arm_body_later_arm_inherits_earlier_rejected_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method P(k: int, x: string?): int "
        "    case k of "
        "      1 => do if x = null then return 0; end; return 10; end, "
        "      2 => do return x.Length; end, "
        "      _ => do return 5; end "
        "    end; "
        "    return -1; "
        "  end P; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// -------------------------------------------------------------------------
// (3) try-body facts leak into catch arms
// (`probe_try_catch_leak.bzl`, SIGSEGV). TryStmt did no local narrowing
// save/restore. Catch-arm entry should be try-entry ∩ after-body; the
// body's body-establishments NEVER enter (a throw may precede the
// establishing guard — probe-confirmed SEGV).
// -------------------------------------------------------------------------
BEZEL_TEST(local_null_narrow_alt_path_2164,
           try_catch_body_fact_leaks_into_catch_rejected_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class MyErr: Exception public constructor() base(\"boom\"); "
        "  end constructor; end MyErr; "
        "class C "
        "  public static method P(x: string?): int "
        "    try "
        "      throw new MyErr(); "
        "      if x = null then return 0; end; "
        "    catch MyErr => return x.Length; "
        "    end; "
        "    return -1; "
        "  end P; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}