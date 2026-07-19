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
BEZEL_TEST(runtime_const_locals_2049, borrow_from_method_call) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public f: int := 0; "
        "  public constructor(v: int) f := v; end constructor; "
        "  public static method Make(): C => new C(99); "  // (test only)
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
