// Codegen — per-instantiation static fields resolve correctly when accessed from
// a DIFFERENT generic class's method body (issue #701).
//
// §22.12: "each distinct instantiation has its own static state."
//
// The pre-fix bugs had two shapes:
//   (a) Cross-named-param: Wrapper<U>.Run() accesses Counter<int>.n.
//       staticFieldGlobal() Path (A) looks up Counter's param "T" in
//       genericSubst_{U→string}; "T" ∉ that map → miss → falls through to the
//       shared template-level bz$Counter$n.
//   (b) Any MemberAccessExpr access inside a generic method body:
//       Path (B) — which extracts concrete type args from the MemberAccessExpr
//       target's TypeRef — was guarded by `!genericSubst_.empty() → else {}`
//       so it was NEVER reached from inside any generic method, even when the
//       access carried its own concrete args (Counter<int>.n).
//
// Both shapes are instances of the same root cause: Path (B) was in an `else`
// branch that was skipped whenever genericSubst_ was non-empty.
//
// The fix (#701): try Path (B) FIRST, unconditionally (extract from the
// MemberAccessExpr target's TypeRef); only fall back to Path (A) when Path (B)
// found nothing (bare unqualified access inheriting the enclosing
// instantiation, e.g. `_count` from Counter<T>'s own method).
//
// Spec references:
//   §22.12 — per-instantiation static state.
//   §14.1  — generic classes are monomorphized per type argument.
//
// Related:
//   #536 — per-instantiation static state via method calls (genericSubst_ path).
//   #697 — direct per-instantiation static access from non-generic context.
//   #684 — per-instantiation static state for imported generics.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

// The canonical #701 repro: Wrapper<U> accesses Counter<int>.n directly.
// Counter's type param is "T"; Wrapper's is "U" — different names so Path (A)
// misses.  Main calls Wrapper<string>.Run() then reads Counter<int>.n.
const char* k701Repro =
    "module M; "
    "class Counter<T> "
    "  public static n: int := 0; "
    "end Counter; "
    "class Wrapper<U> "
    "  public static method Run(): int "
    "    Counter<int>.n := 99; "
    "    return Counter<int>.n; "
    "  end Run; "
    "end Wrapper; "
    "class Program "
    "  public static method Main(): int "
    "    var ignore := Wrapper<string>.Run(); "
    "    return Counter<int>.n; "
    "  end Main; "
    "end Program;";

}  // namespace

// ── Core: the exact #701 repro shape must return 99, not 0 ───────────────────

// §22.12 / #701: Wrapper<string>.Run() writes Counter<int>.n := 99; Main
// then reads Counter<int>.n back.  Must be 99.  Pre-fix: returned 0 because
// the write landed on the shared bz$Counter$n template global while the read
// in Main went to bz$Counter$int$n (or vice versa).
BEZEL_TEST(codegen_generic_static_cross_named_701,
           repro_wrapper_writes_counter_int_main_reads_99_spec_22_12) {
    BEZEL_EXPECT_EQ(run(k701Repro), 99);
}

// §22.12 / #701: Wrapper<string>.Run() return value must also be 99 (the read
// inside the generic method body sees the correct per-instantiation global).
BEZEL_TEST(codegen_generic_static_cross_named_701,
           repro_run_return_value_is_99_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter<T> "
        "  public static n: int := 0; "
        "end Counter; "
        "class Wrapper<U> "
        "  public static method Run(): int "
        "    Counter<int>.n := 99; "
        "    return Counter<int>.n; "
        "  end Run; "
        "end Wrapper; "
        "class Program "
        "  public static method Main(): int "
        "    return Wrapper<string>.Run(); "
        "  end Main; "
        "end Program;"), 99);
}

// ── Isolation: two Counter instantiations stay separate through cross-name access

// §22.12 / #701: Wrapper<U> writes Counter<int>.n and Counter<string>.n to
// distinct values; reads from Main must see separate storage.
BEZEL_TEST(codegen_generic_static_cross_named_701,
           cross_name_two_instantiations_isolated_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter<T> "
        "  public static n: int := 0; "
        "end Counter; "
        "class Wrapper<U> "
        "  public static method Setup() "
        "    Counter<int>.n := 40; "
        "    Counter<string>.n := 2; "
        "  end Setup; "
        "end Wrapper; "
        "class Program "
        "  public static method Main(): int "
        "    Wrapper<bool>.Setup(); "
        "    return Counter<int>.n + Counter<string>.n; "
        "  end Main; "
        "end Program;"), 42);
}

// §22.12 / #701: write to Counter<int>.n from inside Wrapper<U> must NOT
// be visible through Counter<string>.n read from Main.
BEZEL_TEST(codegen_generic_static_cross_named_701,
           cross_name_write_int_not_visible_through_string_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter<T> "
        "  public static n: int := 0; "
        "end Counter; "
        "class Wrapper<U> "
        "  public static method WriteInt() "
        "    Counter<int>.n := 77; "
        "  end WriteInt; "
        "end Wrapper; "
        "class Program "
        "  public static method Main(): int "
        "    Wrapper<string>.WriteInt(); "
        "    return Counter<string>.n; "
        "  end Main; "
        "end Program;"), 0);
}

// ── Generic method body: read/write of own-class statics still works (#536) ──

// §22.12 / #701 + #536 regression: Counter<T>'s own method body accesses bare
// `n` (Path A fallback).  Must still be per-instantiation.
BEZEL_TEST(codegen_generic_static_cross_named_701,
           own_class_bare_access_still_per_instantiation_536_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter<T> "
        "  public static n: int := 0; "
        "  public static method Inc() n := n + 1; end Inc; "
        "  public static method Get(): int return n; end Get; "
        "end Counter; "
        "class Program "
        "  public static method Main(): int "
        "    Counter<int>.Inc(); Counter<int>.Inc(); "
        "    Counter<string>.Inc(); "
        "    return Counter<int>.Get() * 10 + Counter<string>.Get(); "
        "  end Main; "
        "end Program;"), 21);
}

// ── Cross-name + own-class mix: both shapes in one program ───────────────────

// §22.12 / #701: Wrapper<U> writes Counter<int>.n via cross-name access; then
// Counter<int>'s own method Get() reads back (bare access fallback) — both
// must see the SAME per-instantiation global bz$Counter$int$n.
BEZEL_TEST(codegen_generic_static_cross_named_701,
           cross_name_write_own_method_read_same_global_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter<T> "
        "  public static n: int := 0; "
        "  public static method Get(): int return n; end Get; "
        "end Counter; "
        "class Wrapper<U> "
        "  public static method SetInt(v: int) "
        "    Counter<int>.n := v; "
        "  end SetInt; "
        "end Wrapper; "
        "class Program "
        "  public static method Main(): int "
        "    Wrapper<string>.SetInt(55); "
        "    return Counter<int>.Get(); "
        "  end Main; "
        "end Program;"), 55);
}

// ── Multiple wrappers, same counter: consistent view ─────────────────────────

// §22.12 / #701: two different generic wrapper classes (Wrapper1<A> and
// Wrapper2<B>) both write Counter<int>.n; last write wins; Main reads back.
BEZEL_TEST(codegen_generic_static_cross_named_701,
           two_wrappers_write_same_counter_last_wins_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter<T> "
        "  public static n: int := 0; "
        "end Counter; "
        "class Wrapper1<A> "
        "  public static method Set(v: int) Counter<int>.n := v; end Set; "
        "end Wrapper1; "
        "class Wrapper2<B> "
        "  public static method Set(v: int) Counter<int>.n := v; end Set; "
        "end Wrapper2; "
        "class Program "
        "  public static method Main(): int "
        "    Wrapper1<string>.Set(10); "
        "    Wrapper2<bool>.Set(20); "
        "    return Counter<int>.n; "
        "  end Main; "
        "end Program;"), 20);
}

// ── #697 regression: direct external (non-generic) access still works ────────

// §22.12 / #697 regression guard: the fix must not break the original #697
// shape (direct access from non-generic Main).
BEZEL_TEST(codegen_generic_static_cross_named_701,
           regression_697_direct_external_access_still_works_spec_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter<T> "
        "  public static n: int := 0; "
        "end Counter; "
        "class Program "
        "  public static method Main(): int "
        "    Counter<int>.n := 40; "
        "    Counter<string>.n := 2; "
        "    return Counter<int>.n + Counter<string>.n; "
        "  end Main; "
        "end Program;"), 42);
}

// ── #536 regression: method-only access (Path A) still works ─────────────────

// §22.12 / #536 regression guard: generic static accessed only via its own
// class methods (bare unqualified, Path A) must still be per-instantiation.
BEZEL_TEST(codegen_generic_static_cross_named_701,
           regression_536_method_only_access_still_works_spec_22_12) {
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

// #723 / §22.12: a static qualified by the class's OWN generic param inside its
// method (`Box<T>.n`) must resolve to the SAME per-instantiation global as an
// external `Box<int>.n` — not a separate template-level ghost global. Path (B)
// of staticFieldGlobal now resolves the bare param through the active
// substitution. IncQualified() writes Box<int>.n via Box<T>.n; the external
// read sees it: r=1, d=1 → 11.
BEZEL_TEST(generic_static_cross_named_701, self_type_param_qualified_static_723) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  public static n: int := 0; "
        "  public static method IncQualified(): int "
        "    Box<T>.n := Box<T>.n + 1; "
        "    return Box<T>.n; "
        "  end IncQualified; "
        "end Box; "
        "class Program public static method Main(): int "
        "  var r := Box<int>.IncQualified(); "
        "  var d := Box<int>.n; "
        "  return r * 10 + d; "  // 11
        "end Main; end Program;"), 11);
}

// #723: distinct instantiations keep distinct static state (no over-merging).
// Box<int>.n and Box<string>.n are separate; via both qualified and external.
BEZEL_TEST(generic_static_cross_named_701, self_type_param_qualified_distinct_723) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  public static n: int := 0; "
        "  public static method Inc(): int Box<T>.n := Box<T>.n + 1; return Box<T>.n; end Inc; "
        "end Box; "
        "class Program public static method Main(): int "
        "  var a := Box<int>.Inc(); "
        "  var b := Box<string>.Inc(); "
        "  Box<int>.n := Box<int>.n + 5; "
        "  return Box<int>.n * 10 + Box<string>.n; "  // 6*10+1 = 61
        "end Main; end Program;"), 61);
}

// #738 / §22.12: a generic type ARGUMENT that is itself a generic instantiation
// (`Counter<Box<int>>` vs `Counter<Box<string>>`) must get its OWN per-
// instantiation static global. Previously the type-arg mangle and identity
// token both collapsed `Box<int>`/`Box<string>` to `Box` (same llvm `ptr`, same
// declaration token), so both shared one `bz$Counter$Box$n` global and writes
// clobbered each other.
BEZEL_TEST(generic_static_cross_named_701, nested_generic_type_arg_distinct_738) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> public _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  destructor end destructor; end Box; "
        "class Counter<T> public static n: int := 0; end Counter; "
        "class C public static method Main(): int "
        "  Counter<Box<int>>.n := 1; "
        "  Counter<Box<string>>.n := 2; "
        "  Counter<Box<bool>>.n := 3; "
        "  return Counter<Box<int>>.n * 100 + Counter<Box<string>>.n * 10 "
        "         + Counter<Box<bool>>.n; "  // 123
        "end Main; end C;"), 123);
}
