// §22.8 (v0.226) single-assignment of readonly fields (#2187).
//
// §22.8: a `readonly` field is assigned exactly once — the single-assignment
// rule §7.4 ("the field's single assigned value, final by §22.8") and §21.1
// ("member-level runtime single-assignment") depend on. The compiler enforced
// only the PLACEMENT (assignable while in a constructor), not the once-ness, so
// `_f := a; _f := b;` compiled — breaking the #2116 readonly-field narrowing:
// a this-escape helper observes the first value, the ambient non-null fact
// rides into a STORED lambda (sound only if the value is final), and the ctor's
// second assignment nulls the field → the invoked lambda derefs null (probe-
// confirmed `SUMMARY: AddressSanitizer: SEGV ... in lam$0`).
//
// This file is the runtime-value companion: the ACCEPT controls exercise the
// legitimate single-assignment shapes (including conditional-single-across-
// branches — the classic over-reject failure of DA / construction-window
// checks) and confirm they COMPILE and produce the correct value; the REJECT
// cells confirm the double-assignment family is now a §22.8 compile error. The
// lit/FileCheck track (tests/lit/sema/readonly_field_single_assignment_2187__*)
// pins the diagnostic text + citation.

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
// ACCEPT controls — legitimate single-assignment shapes still compile and run.
// (The narrowing reads exercise the #2116 §7.4 feature this fix protects.)
// =========================================================================

// §22.8 (#2187): the #2116 anchor — a single-assignment readonly field narrows
// and its non-null value rides into a stored/invoked lambda. Non-regression:
// the fix must NOT reject the sound single-assignment case. "hey".Length == 3.
BEZEL_TEST(readonly_single_assignment_2187, single_assign_narrowing_lambda) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor(n: string?) _name := n; end constructor; "
        "  destructor end destructor; "
        "  public method Use(): int "
        "    if _name <> null then var f := lambda() => _name.Length; "
        "      return f(); end; return 0; "
        "  end Use; "
        "  public static method Main(): int "
        "    using var c := new C(\"hey\"); return c.Use(); "
        "  end Main; end C;"), 3);
}

// §22.8 (#2187): conditional single-assignment across mutually-exclusive
// `if`/`else` branches — `_f` assigned exactly once on every path. Must be
// ACCEPTED (per-branch reset + union join). c=true ⇒ _f="abcd" ⇒ 4.
BEZEL_TEST(readonly_single_assignment_2187, conditional_single_if_else) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _f: string?; "
        "  public constructor(c: bool) "
        "    if c then _f := \"abcd\"; else _f := \"xy\"; end; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Len(): int "
        "    if _f <> null then return _f.Length; end; return -1; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var c := new C(true); return c.Len(); "
        "  end Main; end C;"), 4);
}

// §22.8 (#2187): a `then` arm that assigns `_f` and diverges (`return`) does
// not collide with the fall-through's single assignment — the union join is
// over LIVE branches only. c=false ⇒ fall-through _f="bbbb" ⇒ 4.
BEZEL_TEST(readonly_single_assignment_2187, divergent_sibling_single) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _f: string?; "
        "  public constructor(c: bool) "
        "    if c then _f := \"aa\"; return; end; _f := \"bbbb\"; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Len(): int "
        "    if _f <> null then return _f.Length; end; return -1; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var c := new C(false); return c.Len(); "
        "  end Main; end C;"), 4);
}

// §22.8 / §22.7 / §7.4 (#2187): observe-then-single-assign is sound. The helper
// observes `_f` while it is the null watermark (establishes nothing, §7.4),
// then the ctor assigns it once. "hello".Length == 5.
BEZEL_TEST(readonly_single_assignment_2187, observe_then_single_assign) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _f: string?; "
        "  public constructor(n: string) Helper(); _f := n; end constructor; "
        "  destructor end destructor; "
        "  public method Helper(): int "
        "    if _f <> null then return _f.Length; end; return 0; "
        "  end Helper; "
        "  public method Len(): int "
        "    if _f <> null then return _f.Length; end; return -1; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var c := new C(\"hello\"); return c.Len(); "
        "  end Main; end C;"), 5);
}

// §22.8 (#2187): a `static readonly` field assigned exactly once in the static
// constructor is accepted and narrows in a static method. "world".Length == 5.
BEZEL_TEST(readonly_single_assignment_2187, static_single_assign) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static readonly _s: string?; "
        "  static constructor() _s := \"world\"; end constructor; "
        "  public static method Get(): int "
        "    if _s <> null then return _s.Length; end; return -1; "
        "  end Get; "
        "  public static method Main(): int return Get(); end Main; end C;"), 5);
}

// =========================================================================
// REJECT cells — the double-assignment family is now a §22.8 compile error.
// =========================================================================

// §22.8 (#2187): the minimal double assignment — the exploit's core.
BEZEL_TEST(readonly_single_assignment_2187, double_assign_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public readonly _f: string?; "
        "  public constructor() _f := \"a\"; _f := null; end constructor; "
        "  destructor end destructor; "
        "  public static method Main(): int return 0; end Main; end C;",
        "cannot assign to readonly field `_f` more than once"));
}

// §22.8 / §22.7 / §7.4 (#2187): the full probe-confirmed SEGV program — a
// double assignment straddling a this-escape that stashes a narrowing lambda.
// Rejecting the double assignment closes the `lam$0` null-deref.
BEZEL_TEST(readonly_single_assignment_2187, this_escape_stored_lambda_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; "
        "class C public readonly _f: string?; public _stash: (() -> int)?; "
        "  public constructor() _f := \"abc\"; Helper(); _f := null; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Helper() "
        "    if _f <> null then _stash := lambda(): int => _f.Length; end; "
        "  end Helper; end C; "
        "class Prog public static method Main(): int "
        "  using var c := new C(); var s := c._stash; "
        "  if s <> null then return s(); end; return -1; end Main; end Prog;",
        "cannot assign to readonly field `_f` more than once"));
}

// §22.8 (#2187): a field initializer plus a body re-assignment is two
// assignments (the initializer runs as step 2, before the body).
BEZEL_TEST(readonly_single_assignment_2187, initializer_then_body_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public readonly _f: string? := \"abc\"; "
        "  public constructor() _f := null; end constructor; "
        "  destructor end destructor; "
        "  public static method Main(): int return 0; end Main; end C;",
        "cannot assign to readonly field `_f` more than once"));
}

// §22.8 (#2187): a readonly field assigned inside a loop body could recur on a
// later iteration — rejected as a potential re-assignment.
BEZEL_TEST(readonly_single_assignment_2187, loop_assign_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public readonly _f: int; "
        "  public constructor() for var i in 0..3 do _f := i; end; end constructor; "
        "  destructor end destructor; "
        "  public static method Main(): int return 0; end Main; end C;",
        "cannot assign to readonly field `_f` inside a loop"));
}
