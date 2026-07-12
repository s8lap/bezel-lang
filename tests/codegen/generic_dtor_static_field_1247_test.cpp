// Codegen — issue #1247: a generic class destructor body that writes a static
// field of the same class (e.g. `GBox<T>._m := GBox<T>._m - 1`) was writing
// to the UNINSTANTIATED (template-level) global when T was a value or string
// type, because the shared type-erased disposer runs WITHOUT an active
// genericSubst_ and staticFieldGlobal() falls through to the template-level
// `bz$GBox$_m` global instead of the per-instantiation `bz$GBox$int$_m`.
//
// Root cause: specializedVtableFor only triggered a per-instantiation Dispose
// (needsSpecDispose) when classHasOwnedTypeParamField(ts) was true, but NOT
// when the class had a user-defined destructor body.  Class-T instantiations
// already happened to trigger the specialization for a different reason (they
// require an owned-T-field disposer), so only value- and string-T
// instantiations were affected.
//
// THE FIX (§28.3/§28.4): in specializedVtableFor, broaden needsSpecDispose:
//   bool needsSpecDispose = classHasOwnedTypeParamField(ts)
//                        || findDestructor(ts) != nullptr;
// This ensures any generic class with a user-defined destructor runs that
// destructor body with genericSubst_ active, so instantiation-dependent
// symbol accesses (static fields, etc.) resolve to the correct
// per-instantiation global.
//
// Spec references:
//   §22.12 — each distinct instantiation has its own static state.
//   §28.3  — destructor body runs at disposal.
//   §28.4  — owned fields are disposed after the destructor body.
//   §28.5  — per-instantiation specialization of the disposer.
//
// Related:
//   #697 — per-instantiation static access from a non-generic context.
//   #536 — per-instantiation static state via method calls (genericSubst_ path).
//   #816 — specialized disposer for owned type-parameter fields.

// AOT MIGRATION (#1769 / #1763): this file runs under the AOT driver (runAot).
// The per-instantiation static counters are all correct under AOT (verified —
// the JIT→AOT divergence #1769 flagged for this file was purely the 8-bit exit
// channel: three tests encode a 1100 observable, which masks to 76 through a
// process exit status). Those three print the observable with Console.WriteLine
// (stdlib §2.1) and assert the captured stdout; the ≤255 observables (10)
// round-trip the exit code.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Observables > 255 cannot round-trip the 8-bit exit channel; the program prints
// the value with Console.WriteLine and the test checks the captured stdout.
std::string runOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// GBox<T> tracks live instances per-instantiation via a static counter.
// The constructor increments the counter; the destructor decrements it.
// Each instantiation has its OWN counter (§22.12).
const char* kGBox =
    "class GBox<T> "
    "  public static _m: int := 0; "
    "  public val: T; "
    "  public constructor(v: T) val := v; GBox<T>._m := GBox<T>._m + 1; end constructor; "
    "  destructor GBox<T>._m := GBox<T>._m - 1; end destructor; "
    "end GBox; ";

}  // namespace

// ── Core: the exact #1247 repro — GBox<int> dtor must decrement _m ───────────

// §28.3/§22.12/#1247: after `using var b := new GBox<int>(42)` goes out of
// scope, GBox<int>._m must be 0 (the destructor decremented it).  Pre-fix
// the destructor wrote to the template-level bz$GBox$_m, not bz$GBox$int$_m,
// so the decrement was silently dropped and the post-scope value remained 1.
// Expected exit code: mid*10 + post = 1*10 + 0 = 10.
BEZEL_TEST(codegen_generic_dtor_static_field_1247,
           gbox_int_dtor_decrements_per_inst_static_spec_22_12_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kGBox +
        "class Program "
        "  public static method Main(): int "
        "    var mid := 0; "
        "    do using var b := new GBox<int>(42); mid := GBox<int>._m; end; "
        "    return mid * 10 + GBox<int>._m; "
        "  end Main; "
        "end Program;"), 10);
}

// ── String-T: dtor writes the per-instantiation counter ───────────────────────

// §28.3/§22.12/#1247: same shape with T=string.  Pre-fix: same wrong-global bug.
BEZEL_TEST(codegen_generic_dtor_static_field_1247,
           gbox_string_dtor_decrements_per_inst_static_spec_22_12_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kGBox +
        "class Program "
        "  public static method Main(): int "
        "    var mid := 0; "
        "    do using var b := new GBox<string>(\"hello\"); mid := GBox<string>._m; end; "
        "    return mid * 10 + GBox<string>._m; "
        "  end Main; "
        "end Program;"), 10);
}

// ── Bool and Char variants ────────────────────────────────────────────────────

// §28.3/§22.12/#1247: GBox<bool> and GBox<char> each have their own counter;
// both must reach 0 after scope exit.
// mid_bool=1, mid_char=1 inside the block; 0, 0 after.
// Return: 1*1000 + 1*100 + 0*10 + 0 = 1100 (full int, not 8-bit truncated).
BEZEL_TEST(codegen_generic_dtor_static_field_1247,
           gbox_bool_char_dtor_decrements_per_inst_static_spec_22_12_28_3) {
    BEZEL_EXPECT_EQ(runOut(
        std::string("module M; ") + kGBox +
        "class Program "
        "  public static method Main(): int "
        "    var midB := 0; var midC := 0; "
        "    do using var b := new GBox<bool>(true); "
        "       using var c := new GBox<char>('x'); "
        "       midB := GBox<bool>._m; midC := GBox<char>._m; "
        "    end; "
        "    Console.WriteLine(midB * 1000 + midC * 100 + GBox<bool>._m * 10 + GBox<char>._m); "
        "    return 0; "
        "  end Main; "
        "end Program;"), "1100\n");
}

// ── Two value-type instantiations: each counter is independent ────────────────

// §22.12/#1247: GBox<int> and GBox<long> are distinct instantiations with
// their own static counters.  Disposing one must not affect the other.
// Return: 1*1000 + 1*100 + 0*10 + 0 = 1100 (full int, not 8-bit truncated).
BEZEL_TEST(codegen_generic_dtor_static_field_1247,
           two_value_instantiations_independent_counters_spec_22_12_28_3) {
    BEZEL_EXPECT_EQ(runOut(
        std::string("module M; ") + kGBox +
        "class Program "
        "  public static method Main(): int "
        "    var midI := 0; var midL := 0; "
        "    do using var bi := new GBox<int>(42); "
        "       using var bl := new GBox<long>(100); "
        "       midI := GBox<int>._m; midL := GBox<long>._m; "
        "    end; "
        "    Console.WriteLine(midI * 1000 + midL * 100 + GBox<int>._m * 10 + GBox<long>._m); "
        "    return 0; "
        "  end Main; "
        "end Program;"), "1100\n");
}

// ── Regression: class-T still gets a specialized disposer (owns field) ────────

// §28.4/§28.5/#1247 regression guard: a generic class with BOTH an owned T
// field AND a static-writing destructor body — the owned field must be disposed
// (ASan clean) AND the static counter must be correct.
// Return: mid=11 (GBox<Trackable>._m=1, Trackable.alive=1 inside), post=00.
// Expected: 11*100 + 0*10 + 0 = 1100 (full int, not 8-bit truncated).
BEZEL_TEST(codegen_generic_dtor_static_field_1247,
           class_t_owned_field_and_static_both_correct_spec_28_4_28_5) {
    BEZEL_EXPECT_EQ(runOut(
        "module M; "
        "class Trackable "
        "  public static alive: int := 0; "
        "  public constructor() Trackable.alive := Trackable.alive + 1; end constructor; "
        "  destructor Trackable.alive := Trackable.alive - 1; end destructor; "
        "end Trackable; "
        "class GBox<T> "
        "  public static _m: int := 0; "
        "  public owned val: T; "
        "  public constructor(owned v: T) val := v; GBox<T>._m := GBox<T>._m + 1; end constructor; "
        "  destructor GBox<T>._m := GBox<T>._m - 1; end destructor; "
        "end GBox; "
        "class Program "
        "  public static method Main(): int "
        "    var mid := 0; "
        "    do using var b := new GBox<Trackable>(new Trackable()); "
        "       mid := GBox<Trackable>._m * 10 + Trackable.alive; "
        "    end; "
        "    Console.WriteLine(mid * 100 + GBox<Trackable>._m * 10 + Trackable.alive); "
        "    return 0; "
        "  end Main; "
        "end Program;"), "1100\n");
}

// ── Regression: non-generic class dtor is unaffected ─────────────────────────

// §28.3 regression guard: a non-generic class with a static counter in its
// destructor must still work correctly (the fix only changes specializedVtableFor
// which is skipped when genericSubst_ is empty — i.e. for non-generic classes).
BEZEL_TEST(codegen_generic_dtor_static_field_1247,
           non_generic_class_dtor_static_unaffected_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter "
        "  public static n: int := 0; "
        "  public constructor() Counter.n := Counter.n + 1; end constructor; "
        "  destructor Counter.n := Counter.n - 1; end destructor; "
        "end Counter; "
        "class Program "
        "  public static method Main(): int "
        "    var mid := 0; "
        "    do using var c := new Counter(); mid := Counter.n; end; "
        "    return mid * 10 + Counter.n; "
        "  end Main; "
        "end Program;"), 10);
}
