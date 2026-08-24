// fix #797 / §2.5 / §22.13: compile-time detection of circular
// static-initialization dependencies. Spec §2.5: "Circular type-level
// dependencies between modules are permitted. Circular static-initialization
// dependencies are not; the compiler detects and rejects them."
//
// checkStaticInitCycles builds the static-init dependency graph — type T depends
// on U when T's static field initializers or static-constructor body reference a
// static member (field/method/property) of U — and rejects any cycle.
//
// #804: the graph is now INTERPROCEDURAL within a module — a dependency reached
// inside a CALLED static method's body or a READ static property's getter body is
// folded into T's edge set (transitive reachability, recursion-guarded). A method
// referenced as a value (not called) is NOT followed (its body does not run), and
// a static property in a WRITE-target position runs its setter, not its getter, so
// the getter is not followed there — both avoid false positives. CROSS-MODULE
// static-init cycles remain a deferred (safe) false negative (#804). These drive
// the real Driver (the pass runs in the driver front-end after typecheck).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir797() {
    return wtScratchDir("wt-797");
}

int compileAndRun(const std::string& source) {
    auto dir = wtDir797();
    std::string stem = unique_stem("bezel_797");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

// True when the source FAILS to compile.
bool compileFails(const std::string& source) {
    auto dir = wtDir797();
    std::string stem = unique_stem("bezel_797_neg");
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

// §2.5: a direct two-type cycle — A's static init reads B's static, B's reads
// A's — is rejected. Before #797 it compiled and produced a silently-wrong value
// (the re-entrant access saw a partially-initialized state).
BEZEL_TEST(static_init_cycles_797, direct_two_type_cycle_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; "
        "class A public static _a: int := B._b + 1; end A; "
        "class B public static _b: int := A._a + 1; end B; "
        "class C public static method Main(): int => A._a; end C;"));
}

// §2.5: a three-type cycle A -> B -> C -> A is rejected (DFS finds the back edge).
BEZEL_TEST(static_init_cycles_797, three_type_cycle_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; "
        "class A public static _a: int := B._b + 1; end A; "
        "class B public static _b: int := C._c + 1; end B; "
        "class C public static _c: int := A._a + 1; end C; "
        "class D public static method Main(): int => A._a; end D;"));
}

// §2.5 / §22.13: a cycle closed through a STATIC METHOD reference — `A._a :=
// B.Get()` triggers B's init (edge A -> B), and B's field init reads A (edge
// B -> A). The reference to the static method is the edge; the method body need
// not be walked.
BEZEL_TEST(static_init_cycles_797, static_method_reference_cycle_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; "
        "class A public static _a: int := B.Get() + 1; end A; "
        "class B "
        "  public static _b: int := A._a + 1; "
        "  public static method Get(): int => _b; "
        "end B; "
        "class C public static method Main(): int => A._a; end C;"));
}

// §2.5 / §22.7: a cycle closed through a STATIC CONSTRUCTOR body — A's cctor
// reads B's static, B's field init reads A's — is rejected (the static-ctor body
// is walked, not just field initializers).
BEZEL_TEST(static_init_cycles_797, static_constructor_cycle_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; "
        "class A "
        "  public static _a: int; "
        "  public static constructor() _a := B._b + 1; end constructor; "
        "end A; "
        "class B public static _b: int := A._a + 1; end B; "
        "class C public static method Main(): int => A._a; end C;"));
}

// §2.5 (no false positive): a NON-cyclic static dependency — A's init reads B's
// static, but B does not reference A — compiles and runs. B is initialized first
// (lazily, on A's first use), so A._a = 5 + 1 = 6.
BEZEL_TEST(static_init_cycles_797, non_cyclic_static_dependency_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class B public static _b: int := 5; end B; "
        "class A public static _a: int := B._b + 1; end A; "
        "class C public static method Main(): int => A._a; end C;"), 6);
}

// §22.13 (no false positive): a type's static initializer referencing its OWN
// earlier static field is textual-order initialization (§22.13), NOT a cycle —
// it compiles and runs. A._y = A._x + 1 = 6.
BEZEL_TEST(static_init_cycles_797, self_reference_textual_order_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class A "
        "  public static _x: int := 5; "
        "  public static _y: int := A._x + 1; "
        "end A; "
        "class C public static method Main(): int => A._y; end C;"), 6);
}

// §2.5 (no false positive): a static method that REFERENCES another type's static
// but is never part of an init cycle (the reference is in a method body invoked
// at runtime, not during static init) compiles. Here Helper.Compute reads
// Config.Value, and nothing references Helper's statics back — no cycle.
BEZEL_TEST(static_init_cycles_797, one_way_static_method_dependency_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Config public static _value: int := 20; end Config; "
        "class Helper public static _base: int := Config._value + 1; end Helper; "
        "class C public static method Main(): int => Helper._base; end C;"), 21);
}

// #804 / §2.5 / §22.13: a TRANSITIVE cycle through a CALLED static method's BODY.
// A's static init calls A.Helper(); Helper's body reads B._b (so A -> B); B's
// init reads A._a (B -> A). This is a real runtime cycle (A's init runs Helper
// which reads B._b, triggering B's init, which reads A._a mid-init) that the
// direct analysis missed (it saw only A -> Helper(self) and B -> A). The
// interprocedural walk folds Helper's body into A's edges and rejects it.
BEZEL_TEST(static_init_cycles_797, transitive_cycle_through_method_body_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; "
        "class A "
        "  public static _a: int := Helper(); "
        "  public static method Helper(): int => B._b; "
        "end A; "
        "class B public static _b: int := A._a + 1; end B; "
        "class C public static method Main(): int => A._a; end C;"));
}

// #804 / §2.5: a transitive cycle through a READ static PROPERTY's getter body —
// A's init reads A.P; P's getter reads B._b (A -> B); B reads A._a (B -> A).
// Reading a property runs its getter, so the getter body is folded into A's set.
BEZEL_TEST(static_init_cycles_797, transitive_cycle_through_getter_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; "
        "class A "
        "  public static _a: int := P; "
        "  public static property P: int => B._b; "
        "end A; "
        "class B public static _b: int := A._a + 1; end B; "
        "class C public static method Main(): int => A._a; end C;"));
}

// #804 (no false positive): an ACYCLIC transitive dependency through a called
// method — A's init calls Helper() which reads Config._value, but nothing
// references A's statics back. No cycle; A._a = Config._value = 20.
BEZEL_TEST(static_init_cycles_797, acyclic_transitive_method_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Config public static _value: int := 20; end Config; "
        "class A "
        "  public static _a: int := Helper(); "
        "  public static method Helper(): int => Config._value; "
        "end A; "
        "class C public static method Main(): int => A._a; end C;"), 20);
}

// #804 (no false positive): an acyclic transitive dependency through a read
// getter — A._a := P, P's getter reads Config._value; no cycle. A._a = 7.
BEZEL_TEST(static_init_cycles_797, acyclic_transitive_getter_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Config public static _value: int := 7; end Config; "
        "class A "
        "  public static _a: int := P; "
        "  public static property P: int => Config._value; "
        "end A; "
        "class C public static method Main(): int => A._a; end C;"), 7);
}

// #804 (recursion guard / no false positive, no hang): a SELF-RECURSIVE static
// method reached during static init must not loop the interprocedural walk and
// must not be flagged as a cycle (the recursion is in the call graph, not a
// static-init dependency cycle). A._a = Rec(3) = 0.
BEZEL_TEST(static_init_cycles_797, self_recursive_static_method_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class A "
        "  public static _a: int := Rec(3); "
        "  public static method Rec(n: int): int => (case n of 0 => 0, _ => Rec(n - 1)); "
        "end A; "
        "class C public static method Main(): int => A._a; end C;"), 0);
}
