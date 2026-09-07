// §22.8 (v0.226) single-assignment of readonly fields on RECORDS (#2207).
//
// §22.8: "A field declared in a class or record may include an initializer"
// and "A field declared `readonly` may be assigned only in a field initializer
// or in a constructor body" — the single-assignment rule §7.4 ("the field's
// single assigned value, final by §22.8") and §21.1 ("member-level runtime
// single-assignment") depend on is type-agnostic. The #2187 fix enforced it
// only for CLASS constructors (the `inClass = enclosing->kind == Class` gate
// in DefiniteAssignment.cpp::checkMember routes class ctors to
// runConstructor/runStaticConstructor, which set roCtorMode_; a record's
// authored ctor fell to the plain da.run path with roCtorMode_ false).
//
// A readonly field of a record could therefore be assigned more than once in
// an authored ctor — and §7.4/#2116 narrowing applies to record fields too —
// so the class-side exploit #2187 closed (a stored-lambda ambient fact
// derefing a value the ctor later nulled → SEGV in `lam$0`) reproduces on a
// record unchanged. This file exercises both the legitimate single-assignment
// ACCEPT controls (the over-reject failure mode must NOT trigger) and the
// REJECT cells (double-assignment family becomes a §22.8 compile error). The
// lit/FileCheck track (tests/lit/sema/readonly_record_single_assignment_2207__*)
// pins the diagnostic text + citation; the gtest harness here pins the
// runtime semantics and the spec-cited REJECT text.
//
// Per §22.7 / §7.9, as of fix #4001 and fix #4014 (this comment previously
// claimed neither obligation reaches records at all — both claims were
// false premises, corrected in turn): a record constructor DOES carry its
// own §7.9 field-completion fall-off check (#4001) and its own §22.7 rule-1
// this-observation gate (#4014) — rule 1's hazard (an ordinary instance
// call/property observation of `this` before a field it reads is assigned)
// is independent of inheritance and applies to a record's own sealed,
// non-virtual methods exactly as it does to a class's. Only §22.7 rule 2
// (no virtual dispatch from a non-sealed class's constructor) stays
// genuinely class-only — records are implicitly sealed with no base class
// (§9.6), so that footgun cannot arise for one. Every field this file's
// records declare is excluded from §7.9-strictness by one of two DIFFERENT
// mechanisms, never by fix #4014 itself: `_f: string?`/`_stash: (() -> int)?`
// are excluded because they are **nullable** (§7.9 only requires explicit
// initialization of non-nullable storage), while `_f: int` (the
// `double_assign_rejected`/`loop_assign_rejected` cases below) is excluded
// because `int` is a **defaultable value type** (§17.11) — non-nullable, but
// exempt from §7.9's must-initialize rule because it has a valid zero value
// to fall back on. Neither shape is §7.9-strict, so `fieldsNeedingInit_` is
// empty for every record in this file and rule 1's gate has nothing to
// observe in any of it — the readonly single-assignment tracker (the
// once-ness §22.8/#2187 enforces) is the only mechanism exercised below,
// unaffected by either #4001 or #4014.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// True iff sema rejected `src` with a diagnostic containing `needle`.
bool rejectedWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =========================================================================
// ACCEPT controls — legitimate single-assignment shapes still compile and
// run on records. (The narrowing reads exercise the #2116 §7.4 feature the
// fix protects — also applies to record fields per §7.4 "instance or
// static" field.)
// =========================================================================

// §22.8 (#2207): a single-assignment readonly record field narrows and its
// non-null value rides into a stored/invoked lambda. Non-regression: the
// fix must NOT reject the sound single-assignment case. "hey".Length == 3.
BEZEL_TEST(readonly_record_single_assignment_2207, single_assign_narrowing_lambda) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  public method Use(): int "
        "    if _name <> null then var f := lambda() => _name.Length; "
        "      return f(); end; return 0; "
        "  end Use; "
        "  public static method Main(): int "
        "    using var r := new R(\"hey\"); return r.Use(); "
        "  end Main; end R;"), 3);
}

// §22.8 (#2207): conditional single-assignment across mutually-exclusive
// `if`/`else` branches on a RECORD — `_f` assigned exactly once on every
// path. Must be ACCEPTED (per-branch reset + union join, the #2187 shape
// applied to records). c=true ⇒ _f="abcd" ⇒ 4.
BEZEL_TEST(readonly_record_single_assignment_2207, conditional_single_if_else) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R public readonly _f: string?; "
        "  public constructor(c: bool) "
        "    if c then _f := \"abcd\"; else _f := \"xy\"; end; "
        "  end constructor; "
        "  public method Len(): int "
        "    if _f <> null then return _f.Length; end; return -1; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var r := new R(true); return r.Len(); "
        "  end Main; end R;"), 4);
}

// §22.8 (#2207): the #2116 narrowing anchor on a record — the entire
// problem this fix exists to close. The ctor assigns `_f` exactly once
// (here, taking a parameter `n`); the record's `Len` method then narrows
// `_f` via §7.4 and reads `_f.Length`. Records are value types, so the
// `this`-escape shape from the #2187 class-side exploit does not propagate
// — but the readonly single-assignment rule (§7.4/#2116 narrowing depends
// on it) still applies, and a double-assign in the ctor is the §22.8
// violation this fix rejects. "world".Length == 5.
BEZEL_TEST(readonly_record_single_assignment_2207, observe_then_single_assign) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R public readonly _f: string?; "
        "  public constructor(n: string) _f := n; end constructor; "
        "  public method Len(): int "
        "    if _f <> null then return _f.Length; end; return -1; "
        "  end Len; "
        "  public static method Main(): int "
        "    var r := new R(\"world\"); return r.Len(); "
        "  end Main; end R;"), 5);
}

// =========================================================================
// REJECT cells — the double-assignment family on a RECORD is a §22.8 error.
// =========================================================================

// §22.8 (#2207): the minimal double-assign on a record (the issue's
// accept-side confirmation probe).
BEZEL_TEST(readonly_record_single_assignment_2207, double_assign_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; record R public readonly _f: int; "
        "  public constructor(x: int) _f := x; _f := 0; end constructor; "
        "  public static method Main(): int return 0; end Main; end R;",
        "cannot assign to readonly field `_f` more than once"));
}

// §22.8 / §22.7 / §7.4 (#2207): the exact probe from the issue — a record's
// authored ctor double-assigns a readonly field across a this-escape helper
// that stashes a narrowing lambda. The exploit (SEGV in `lam$0` on main and
// on the #2187 branch alike) is closed by rejecting the second assignment.
// NB: as of fix #4014, §22.7 rule 1's this-observation gate DOES run on
// records — but `_f` and `_stash` are both **nullable**, so neither is a
// §7.9-strict field and rule 1's gate (which only ever inspects
// §7.9-strict fields) has nothing to observe on this program; the
// rejection comes from the readonly once-check alone, unaffected by #4014.
BEZEL_TEST(readonly_record_single_assignment_2207, this_escape_stored_lambda_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; "
        "record R public readonly _f: string?; public _stash: (() -> int)?; "
        "  public constructor() _f := \"abc\"; Helper(); _f := null; "
        "  end constructor; "
        "  public method Helper() "
        "    if _f <> null then _stash := lambda(): int => _f.Length; end; "
        "  end Helper; end R; "
        "class Prog public static method Main(): int "
        "  using var r := new R(); var s := r._stash; "
        "  if s <> null then return s(); end; return -1; end Main; end Prog;",
        "cannot assign to readonly field `_f` more than once"));
}

// §22.8 (#2207): a record's field initializer plus a body re-assignment is
// two assignments (the initializer runs as step 2, before the body). The
// `roCtorMode_` + `readonlyMaybeAssigned_` seed at body entry rejects this.
BEZEL_TEST(readonly_record_single_assignment_2207, initializer_then_body_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; record R public readonly _f: string? := \"abc\"; "
        "  public constructor() _f := null; end constructor; "
        "  public static method Main(): int return 0; end Main; end R;",
        "cannot assign to readonly field `_f` more than once"));
}

// §22.8 (#2207): a readonly field assigned inside a loop body on a record
// could recur on a later iteration — rejected as a potential re-assignment,
// same shape as #2187's class-side check, now applied to records.
BEZEL_TEST(readonly_record_single_assignment_2207, loop_assign_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; record R public readonly _f: int; "
        "  public constructor() for var i in 0..3 do _f := i; end; end constructor; "
        "  public static method Main(): int return 0; end Main; end R;",
        "cannot assign to readonly field `_f` inside a loop"));
}