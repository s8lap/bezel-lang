// Spec-derived tests for issue #2049 — runtime `const` locals (0049 train A),
// per spec §21.1 v0.201. A `const` local whose initializer is NOT a §17.14
// constant expression OR whose declared/inferred type is not a primitive
// value type / `string` is a RUNTIME const: a single-assignment, never-owned
// binding. Admitted initializers include value-typed expressions, `string`
// and other runtime-managed values, `null`, and borrows. See §21.1, §17.14,
// §28.3 ("Ownership" subsection of §21.1).
//
// These tests assert the SLICE 1 deliverables:
//   * A runtime const computes its value at runtime and reads correctly.
//   * A runtime const of each admissible initializer kind (int from a non-
//     constant expression; a borrow; null) compiles and runs.
//   * The pre-v0.201 "must have primitive type" reject is GONE for locals —
//     `const X: T := null;` is now a valid runtime const.
//
// Single-assignment enforcement and the never-owned ownership rule are
// delivered by Slices 2 and 3 respectively (separate test files).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

namespace fs = std::filesystem;

// True when `source` FAILS to compile (a diagnostic is raised — NOT a crash).
bool compileFails(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_2049rc_" + std::to_string(counter++);
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}

}  // namespace

// §21.1 v0.201 (#2049): a runtime const of `int` initialized from a non-
// constant expression computes its value at runtime and reads correctly.
// Spec example: `const n := items.Count + 1;` (§21.1).
BEZEL_TEST(runtime_const_locals_2049, int_from_var_plus_literal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int := 41; "
        "    const K := n + 1; "       // runtime const: int (n is non-const)
        "    return K; "               // 42
        "  end Main; "
        "end C;"), 42);
}

// §21.1: a runtime const may be read multiple times — single-assignment,
// not single-read.
BEZEL_TEST(runtime_const_locals_2049, int_read_multiple_times) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int := 7; "
        "    const K := n * 3; "         // 21
        "    var s: int := 0; "
        "    var i: int := 0; "
        "    while i < K do s += i; i += 1; end; "  // sum 0..20 = 210
        "    return s; "
        "  end Main; "
        "end C;"), 210);
}

// §21.1: a runtime const of reference type — a borrow. `r` aliases the
// owned local `c`; reads through `r` observe the same object.
BEZEL_TEST(runtime_const_locals_2049, borrow_const_reads_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public f: int := 0; "
        "  public constructor(v: int) f := v; end constructor; "
        "  public static method Main(): int "
        "    using var c := new C(42); "
        "    const r := c; "            // runtime const: a borrow of c
        "    return r.f; "              // 42
        "  end Main; "
        "end C;"), 42);
}

// §21.1: a runtime const of `string` (runtime-managed per §28.9) is admitted
// — the binding is never-owned by the runtime-managed exemption.
BEZEL_TEST(runtime_const_locals_2049, string_const_from_concat) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var prefix: string := \"Hello, \"; "
        "    const greeting := prefix + \"World!\"; "  // runtime string
        "    return greeting.Length; "                 // 13
        "  end Main; "
        "end C;"), 13);
}

// §21.1: a runtime const initialized from a method-call result. The plain
// (non-`owned`) call returns a borrow; the const local carries that borrow.
// v0.201 (#2048): the fresh-producing factory is spelled `owned C` per the
// declaration-site static-return rule.
BEZEL_TEST(runtime_const_locals_2049, borrow_from_method_call) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public f: int := 0; "
        "  public constructor(v: int) f := v; end constructor; "
        "  public static method Make(): owned C => new C(99); "  // (test only)
        "  public static method Consume(c: C): int => c.f; "
        "  public static method Main(): int "
        "    using var c := new C(99); "
        "    const r := c; "            // borrow of c (no owned production)
        "    return Consume(r); "       // 99
        "  end Main; "
        "end C;"), 99);
}

// §21.1: a permanently-null const local. `const X: T? := null;` is admitted
// — the binding is then permanently null. (Spec: "`null` ... is admissible".)
BEZEL_TEST(runtime_const_locals_2049, permanently_null_const) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    const X: C? := null; "
        "    if X = null then return 1; end; "  // X is permanently null
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §21.1 v0.201 (#2049): the pre-v0.201 "const must be primitive or string"
// reject is GONE for locals — `const X: T := null;` (T a class type) is now
// a valid runtime const, NOT a compile error. (Old §22.12-style reject no
// longer fires for locals.)
BEZEL_TEST(runtime_const_locals_2049, named_type_const_no_longer_rejected) {
    BEZEL_EXPECT_FALSE(compileFails(
        "module M; class T end T; class C "
        "  public static method Main(): int "
        "    const X: T := null; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// §17.14 / §21.1: a compile-time-constant local const (constant expression
// of primitive type) keeps its existing semantics — non-regression.
BEZEL_TEST(runtime_const_locals_2049, compile_time_const_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    const N := 40 + 2; "       // compile-time const: int (§17.14)
        "    return N; "                // 42
        "  end Main; "
        "end C;"), 42);
}

// §17.14: a compile-time-constant local const still folds in constant
// positions (inline-array size) — non-regression for the existing semantics.
BEZEL_TEST(runtime_const_locals_2049, compile_time_const_array_size_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    const SZ: int := 3; "
        "    var arr: [SZ]int := [10, 20, 30]; "
        "    return arr[2]; "           // 30
        "  end Main; "
        "end C;"), 30);
}

// §21.1 v0.201 (#2049): a bare `null` initializer without a type annotation
// gives the const no determinable type (parallel to §7.1's `var x := null;`
// rule). The annotation is REQUIRED — `const X := null;` is rejected.
BEZEL_TEST(runtime_const_locals_2049, bare_null_without_annotation_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    const X := null; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// =========================================================================
// Single-assignment enforcement — §21.1 v0.201 (#2049) Slice 2.
// A `const` local of either classification is not an assignable location:
// assignment, compound assignment, `++`/`--`, ref/out binding, and mutable
// lambda capture are all compile errors naming the declaration. The same
// forms are admitted on a `var` local — non-regression confirmations below.
// =========================================================================

// §21.1: assignment to a const local is a compile error.
BEZEL_TEST(runtime_const_locals_2049, assign_to_const_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    const K := 5; "
        "    K := 10; "
        "    return K; "
        "  end Main; "
        "end C;"));
}

// §21.1: compound assignment to a const local is a compile error.
BEZEL_TEST(runtime_const_locals_2049, compound_assign_to_const_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    const K := 5; "
        "    K += 1; "
        "    return K; "
        "  end Main; "
        "end C;"));
}

// §21.1: `??=` to a const local is a compile error.
BEZEL_TEST(runtime_const_locals_2049, coalesce_assign_to_const_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    const K: int? := 5; "
        "    K ??= 1; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// §21.1: `++` on a const local is a compile error.
BEZEL_TEST(runtime_const_locals_2049, increment_const_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    const K := 5; "
        "    K++; "
        "    return K; "
        "  end Main; "
        "end C;"));
}

// §21.1: `--` on a const local is a compile error.
BEZEL_TEST(runtime_const_locals_2049, decrement_const_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    const K := 5; "
        "    K--; "
        "    return K; "
        "  end Main; "
        "end C;"));
}

// §21.1 / §22.3: binding a const local to a `ref` parameter is a compile
// error.
BEZEL_TEST(runtime_const_locals_2049, ref_const_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method TakeRef(ref x: int): int => x; "
        "  public static method Main(): int "
        "    const K := 5; "
        "    return TakeRef(ref K); "
        "  end Main; "
        "end C;"));
}

// §21.1 / §22.3: binding a const local to an `out` parameter is a compile
// error.
BEZEL_TEST(runtime_const_locals_2049, out_const_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method TakeOut(out x: int): bool "
        "    x := 7; return true; "
        "  end TakeOut; "
        "  public static method Main(): int "
        "    const K := 5; "
        "    if TakeOut(out K) then return K; end; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// §21.1: mutable-capture of a const local in a lambda body is a compile
// error (it requires the assignment form, which is itself rejected). A
// READ capture is fine — that is the lambda's `() => K;` form.
BEZEL_TEST(runtime_const_locals_2049, mutable_capture_const_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    const K := 5; "
        "    var f := lambda() => do K := 10; end; "
        "    return K; "
        "  end Main; "
        "end C;"));
}

// §21.1: a runtime const read from inside a lambda body is fine — a const
// local may be captured by READ (the immutable form). The lambda observes
// the const's value at the time it is invoked.
BEZEL_TEST(runtime_const_locals_2049, read_capture_const_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n := 40; "
        "    const K := n + 2; "            // runtime const: 42
        "    var f := lambda() => K; "            // capture by read
        "    return f(); "                  // 42
        "  end Main; "
        "end C;"), 42);
}

// §7.4 v0.201 (#2049): a const local's narrowing is NEVER invalidated — a
// null test narrows it for the remainder of its scope, across calls. The
// field-copy idiom of §7.4 is airtight when the copy is `const`. The
// initializer here is a borrow (per §21.1 ownership: a const local must
// not take an owned production — Slice 3), so the test exercises the
// narrowing path on a never-owned binding.
BEZEL_TEST(runtime_const_locals_2049, narrowing_persists_across_call) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public f: int := 0; "
        "  public constructor(v: int) f := v; end constructor; "
        "  public static method Helper() Console.WriteLine(\"x\"); end Helper; "
        "  public static method Main(): int "
        "    using var c := new C(7); "
        "    const X: C? := c; "           // runtime const: borrow of c, non-null
        "    if X = null then return 99; end; "
        "    Helper(); "                   // X stays narrowed (const local)
        "    return X.f; "                 // 7 — no `?.` needed
        "  end Main; "
        "end C;"), 7);
}

// =========================================================================
// Non-regression: the same forms on a `var` local must keep working.
// =========================================================================

// §21.1 (non-regression): `var` locals remain assignable — same forms that
// are rejected on `const` are admitted on `var`.
BEZEL_TEST(runtime_const_locals_2049, var_local_assign_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var k := 5; "
        "    k := 10; "
        "    k += 1; "
        "    k++; "
        "    return k; "                   // 12
        "  end Main; "
        "end C;"), 12);
}

// =========================================================================
// Never-owned ownership rule — §21.1 v0.201 (#2049) Slice 3.
// A const local must not take an owned production (§28.3): the binding
// never takes a disposal obligation. The rejected forms — fresh `new`,
// owned-returning call, `move`, … — name the rule and the `using var`
// remedy. Admitted forms — value-typed expressions, `string`, `null`,
// borrows — were exercised in Slice 1 above; the tests below focus on
// the rejected forms.
// =========================================================================

// §21.1 / §28.3: a const local initialized from a fresh `new` is rejected
// — the binding would take a disposal obligation it cannot discharge.
BEZEL_TEST(runtime_const_locals_2049, owned_new_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    const r := new C(); "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// §21.1 / §28.3: a const local initialized from an owned-returning call is
// rejected. The remedy is `using var` (the binding takes the obligation
// and disposes at scope exit).
BEZEL_TEST(runtime_const_locals_2049, owned_call_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public f: int := 0; "
        "  public static method Make(): owned C => new C(); "
        "  public static method Main(): int "
        "    const r := Make(); "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// §21.1 / §28.8: a const local initialized from a `move` of an owned
// binding is rejected — `move` transfers the obligation, which the const
// cannot take.
BEZEL_TEST(runtime_const_locals_2049, owned_move_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    const r := move c; "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// §21.1 / §28.3: the NEVER-OWNED rule admits a const local bound to a
// borrow (read of an existing owned binding). `using var c := new C();
// const r := c;` is the spec's borrow-const idiom — `r` is a const borrow
// of `c`, and reads through `r` observe `c`'s object.
BEZEL_TEST(runtime_const_locals_2049, borrow_initializer_admitted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public f: int := 0; "
        "  public constructor(v: int) f := v; end constructor; "
        "  public static method Main(): int "
        "    using var c := new C(42); "
        "    const r := c; "              // borrow of c (never-owned)
        "    return r.f; "                // 42
        "  end Main; "
        "end C;"), 42);
}

// §21.1 (v0.298) / #4287: a `const` local bound to a marked-transfer
// `move` of a BUILT-IN class type (Array<int>, Dictionary<…>, the FFI
// buffers, …) is rejected. §21.1's text names "a `move` (the marked
// transfer `const y := move x;` included — not a form)" as a rejected
// initializer form. The fix: the `constInitializerRejected` predicate now
// catches any UnaryExpr with `op == "move"` (the producerState disjunct
// alone misses built-ins because their declarations are null — see
// Ownership.cpp:~5036 vs. ~4628). Pre-fix the program compiled clean
// (zero diagnostics) — the issue's measured repro on `rev/4268` @ 60e57be.
// The diagnostic must (a) cite §21.1 and (b) name the `using var` remedy
// (§21.1's mandated wording). FAILS WITHOUT the fix (compileFails is false).
BEZEL_TEST(runtime_const_locals_2049, const_move_of_builtin_array_rejected_4287) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(2); a.Add(1); "
        "    const c := move a; "       // §21.1: a move is a rejected form
        "    return c.Count; "
        "  end Main; "
        "end C;"));
}

// §21.1 (v0.298) / #4287: a `const` local bound to a `move` of a
// borrowing-collection value (Dictionary, Set, Stack, Queue, LinkedList)
// is rejected for the same reason as the Array<int> case — the move form
// is rejected irrespective of the moved type per §21.1's text. A second
// data point (beyond the Array repro) — the predicate catches the shape,
// not the type. FAILS WITHOUT the fix.
BEZEL_TEST(runtime_const_locals_2049,
           const_move_of_builtin_dictionary_rejected_4287) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d[\"k\"] := 7; "
        "    const c := move d; "       // §21.1: a move is a rejected form
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// §21.1 (v0.298) / #4287: the SAME move form on a VALUE-typed operand
// (a record) is still rejected — §21.1's "a `move`" qualifier does not
// exempt value-typed moves. The const binding never takes a transfer
// regardless of the moved type. FAILS WITHOUT the fix.
BEZEL_TEST(runtime_const_locals_2049,
           const_move_of_value_typed_record_rejected_4287) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; record R(n: int); class C "
        "  public static method Main(): int "
        "    var r := new R(7); "
        "    const c := move r; "       // §21.1: a move is a rejected form
        "    return c.n; "
        "  end Main; "
        "end C;"));
}

// §21.1 (v0.298) / #4287 — ACCEPT CONTROL: a const bound to a plain
// BORROW of a built-in class (`const c := a;` with no `move`) keeps
// working — the rule rejects a `move` form specifically, never the bare
// read. This is the spec's borrow-const idiom on a built-in class shape
// (the same shape the false-accept path used). The fix MUST NOT widen
// to a bare borrow; that is an accept-direction over-reject.
BEZEL_TEST(runtime_const_locals_2049,
           const_borrow_of_builtin_array_admitted_4287_control) {
    BEZEL_EXPECT_FALSE(compileFails(
        "module M; class C "
        "  public static method Main(): int "
        "    var a := new Array<int>(2); a.Add(1); "
        "    const c := a; "            // borrow of a (no move) — admitted
        "    return c.Count; "
        "  end Main; "
        "end C;"));
}
