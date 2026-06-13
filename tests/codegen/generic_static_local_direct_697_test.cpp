// Codegen — LOCAL generic-class static fields are per-instantiation when accessed
// DIRECTLY (with explicit type args) from a non-generic context (issue #697).
//
// §22.12: "each distinct instantiation has its own static state."
//
// The pre-fix bug: `Counter<int>.n := 40; Counter<string>.n := 2;
// return Counter<int>.n + Counter<string>.n` returned 4 instead of 42, because
// both qualified accesses resolved to the SAME template-level global
// (`bz$Counter$n`).  The per-instantiation path in staticFieldGlobal() was
// guarded by `!genericSubst_.empty()`, which is only true when the access
// happens INSIDE a monomorphized generic method body.  Direct external access
// from a non-generic method (e.g. static Main) never set genericSubst_, so it
// fell through to the shared global.
//
// The fix: when genericSubst_ is empty, staticFieldGlobal() inspects the
// expression itself — for a `Counter<int>.n` MemberAccessExpr the target
// IdentifierExpr carries a sema-resolved Named TypeRef with genericArgs.
// Those args are extracted and used to build the per-instantiation key and
// global name (`bz$Counter$int$n`, `bz$Counter$string$n`), restoring correct
// §22.12 isolation.
//
// Spec references:
//   §22.12 — per-instantiation static state.
//   §14.1  — generic classes are monomorphized per type argument.
//
// Related:
//   #536 — per-instantiation static state via method calls (genericSubst_ path).
//   #684 — per-instantiation static state for IMPORTED generics.
//   #696 — sema diagnostic for bare unqualified generic static access outside class.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

bezel::test::JitResult jit(const std::string& prog) {
    return runJit(prog);
}

// A generic class with a public static field, so the field can be directly
// written/read from outside the class body using `Counter<T>.n` syntax.
const char* kCounterPub =
    "module M; "
    "class Counter<T> "
    "  public static n: int := 0; "
    "end Counter; ";

// A two-param generic class with a public static field.
const char* kPair =
    "module M; "
    "class Pair<A, B> "
    "  public static count: int := 0; "
    "end Pair; ";

}  // namespace

// ── Core: direct external write / read isolates per instantiation ─────────────

// §22.12 / #697: the g696b.bzl probe shape — write 40 to Counter<int>.n and 2
// to Counter<string>.n directly from Main, then add them; must be 42.
BEZEL_TEST(codegen_generic_static_local_direct_697,
           direct_write_two_instantiations_sum_is_42_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kCounterPub) +
        "class Program "
        "  public static method Main(): int "
        "    Counter<int>.n := 40; "
        "    Counter<string>.n := 2; "
        "    return Counter<int>.n + Counter<string>.n; "
        "  end Main; "
        "end Program;"), 42);
}

// §22.12 / #697: write to Counter<int>.n must NOT be visible through Counter<string>.n.
BEZEL_TEST(codegen_generic_static_local_direct_697,
           write_int_not_visible_through_string_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kCounterPub) +
        "class Program "
        "  public static method Main(): int "
        "    Counter<int>.n := 99; "
        "    return Counter<string>.n; "
        "  end Main; "
        "end Program;"), 0);
}

// §22.12 / #697: write to Counter<string>.n must NOT be visible through Counter<int>.n.
BEZEL_TEST(codegen_generic_static_local_direct_697,
           write_string_not_visible_through_int_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kCounterPub) +
        "class Program "
        "  public static method Main(): int "
        "    Counter<string>.n := 77; "
        "    return Counter<int>.n; "
        "  end Main; "
        "end Program;"), 0);
}

// §22.12 / #697: three instantiations (int, string, bool) — each gets its own
// direct-write value; reads are independently isolated.
BEZEL_TEST(codegen_generic_static_local_direct_697,
           three_instantiations_direct_write_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kCounterPub) +
        "class Program "
        "  public static method Main(): int "
        "    Counter<int>.n := 10; "
        "    Counter<string>.n := 20; "
        "    Counter<bool>.n := 30; "
        "    if Counter<int>.n = 10 and "
        "       Counter<string>.n = 20 and "
        "       Counter<bool>.n = 30 "
        "    then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// ── Static init: per-instantiation globals carry the correct initializer ───────

// §22.12 / §22.13 / #697: a static field initialized to a non-zero literal
// (int initializer → baked into the per-instantiation global's initializer by
// constInitFor) must read back correctly for each fresh instantiation without
// any explicit write.
BEZEL_TEST(codegen_generic_static_local_direct_697,
           initializer_baked_per_instantiation_spec_22_12_22_13) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Tagged<T> "
        "  public static version: int := 7; "
        "end Tagged; "
        "class Program "
        "  public static method Main(): int "
        "    if Tagged<int>.version = 7 and Tagged<string>.version = 7 "
        "    then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// §22.12 / §22.13 / #697: after writing a per-instantiation global, the OTHER
// instantiation's initializer value is unaffected.
BEZEL_TEST(codegen_generic_static_local_direct_697,
           write_does_not_corrupt_other_initializer_spec_22_12_22_13) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Tagged<T> "
        "  public static version: int := 5; "
        "end Tagged; "
        "class Program "
        "  public static method Main(): int "
        "    Tagged<int>.version := 99; "
        "    if Tagged<int>.version = 99 and Tagged<string>.version = 5 "
        "    then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;"), 1);
}

// ── #696 sema diagnostic still fires (regression guard) ──────────────────────

// §22.12 / #696 regression: bare `Counter.n` (no type args) from outside the
// class body must still be rejected after the #697 codegen fix.
BEZEL_TEST(codegen_generic_static_local_direct_697,
           bare_unqualified_access_still_diagnoses_spec_22_12) {
    auto r = jit(
        "module M; "
        "class Counter<T> "
        "  public static n: int := 0; "
        "end Counter; "
        "class Program "
        "  public static method Main(): int "
        "    return Counter.n; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// ── Mix: direct external write then method-body read (both paths agree) ───────

// §22.12 / #697: write Counter<int>.n := 5 directly from Main (path B, no
// genericSubst_), then read it back via a static method body (path A, via
// genericSubst_) — both must see the same per-instantiation global.
BEZEL_TEST(codegen_generic_static_local_direct_697,
           external_write_then_method_read_same_global_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter<T> "
        "  public static n: int := 0; "
        "  public static method Get(): int return n; end Get; "
        "end Counter; "
        "class Program "
        "  public static method Main(): int "
        "    Counter<int>.n := 13; "
        "    Counter<string>.n := 7; "
        "    return Counter<int>.Get() + Counter<string>.Get(); "
        "  end Main; "
        "end Program;"), 20);
}

// ── #684 regression guard: imported-generic statics still work ───────────────

// §22.12 / #684 regression guard: the fix must not disturb the imported-generic
// per-instantiation path (path (A) guard, genericSubst_ non-empty).  A local
// generic class whose statics are accessed only via methods (the #536 shape)
// must continue to work correctly.
BEZEL_TEST(codegen_generic_static_local_direct_697,
           method_only_access_still_works_536_regression_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter<T> "
        "  private static _c: int := 0; "
        "  public static method Inc() _c := _c + 1; end Inc; "
        "  public static method Get(): int return _c; end Get; "
        "end Counter; "
        "class Program "
        "  public static method Main(): int "
        "    Counter<int>.Inc(); Counter<int>.Inc(); "
        "    Counter<string>.Inc(); "
        "    return Counter<int>.Get() * 10 + Counter<string>.Get(); "
        "  end Main; "
        "end Program;"), 21);
}

// ── Two-param generic: both params form the key ───────────────────────────────

// §22.12 / #697: Pair<int, string> and Pair<string, int> are distinct
// instantiations; direct writes must be isolated.
BEZEL_TEST(codegen_generic_static_local_direct_697,
           two_param_generic_direct_write_isolated_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kPair) +
        "class Program "
        "  public static method Main(): int "
        "    Pair<int, string>.count := 3; "
        "    Pair<string, int>.count := 9; "
        "    return Pair<int, string>.count + Pair<string, int>.count; "
        "  end Main; "
        "end Program;"), 12);
}

// ── Non-generic statics unaffected ───────────────────────────────────────────

// §22 regression guard: a non-generic class's static field accessed directly
// must still accumulate correctly (CG12 base path unaffected by #697).
BEZEL_TEST(codegen_generic_static_local_direct_697,
           nongeneric_static_field_direct_access_unaffected_spec_22) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter "
        "  public static n: int := 0; "
        "end Counter; "
        "class Program "
        "  public static method Main(): int "
        "    Counter.n := 42; "
        "    return Counter.n; "
        "  end Main; "
        "end Program;"), 42);
}
