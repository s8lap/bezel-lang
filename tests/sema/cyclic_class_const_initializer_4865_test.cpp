// fix #4865 / §17.14 / §22.12 (v0.307): a class-level `const` initializer
// must be a compile-time constant expression whose value is well-defined; a
// self/mutually-referential initializer has no value. Sema §17.14 folds
// terminate on their own depth bound but the cycle escapes the fold through
// the bare-identifier / qualified-const-reference path: `constMemberValue`
// re-lowers the referenced const's initializer, which re-resolves to its own
// AST decl, which re-enters — unbounded recursion that overflows the process
// stack (SIGSEGV, codegen). Three repro shapes share the bug:
//
//   1. SAME-class DOUBLE pair: `K1 := K2; K2 := K1;` (int pair: `+1`)
//   2. CROSS-class pair: `A.K := B.K; B.K := A.K;`
//   3. UNUSED pair (no `Main`, no read) — silently produced no diagnostic and
//      `bezelc -c` exited 0; the codegen path wasn't even reached, so the
//      SIGSEGV only fired under the use.
//
// `checkConstInitCycles` (next to `checkStaticInitCycles`) builds the graph
// over LOCAL `ConstMemberSymbol`s — an edge K1 -> K2 iff K1's initializer
// references K2 — and rejects each distinct cycle once. Imported consts
// fold via textual value, not the AST path, so they don't enter the graph
// (cross-module cycles are a deferred dimension; same posture as §22.13
// cycles, family-static-init).
//
// Each REJECT row asserts `r.ran == false` (no compile/run) AND the
// diagnostic substring "cyclic class-level const initializer" AND that the
// pre-fix shape (`exitCode == 139`, signal 11) is absent — a crash reports
// through the harness as `r.ran == false` with the harness's "killed
// (compile hang / runaway front-end)" message, NOT the cycle diagnostic, so
// the substring assertion catches the no-crash half. ACCEPT rows for the
// forward-reference (non-cycle) shapes confirm no false rejects.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasDiag(const bezel::test::JitResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// A clean reject: did NOT crash (no SIGSEGV — pre-fix shape) AND has the
// expected diagnostic substring.
bool rejectedWith(const bezel::test::JitResult& r, std::string_view needle) {
    if (r.ran) return false;                   // compile succeeded — reject?
    if (r.exitCode == 139) return false;       // SIGSEGV — pre-fix shape
    return hasDiag(r, needle);
}

}  // namespace

// §22.12 / §17.14: same-class double pair (issue's primary repro). The
// pre-fix shape is `bezelc -c` SIGSEGV (exit 139) in `lowerExprInner`
// → `constMemberValue` mutual recursion.
BEZEL_TEST(cyclic_class_const_initializer_4865,
           same_class_double_pair_rejected) {
    auto r = runAot(
        "module M; "
        "class A "
        "public static const K1: double := K2; "
        "public static const K2: double := K1; "
        "end A; "
        "class C "
        "public static method Main(): int "
        "Console.WriteLine(int(A.K1)); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "cyclic class-level const initializer"));
}

// §22.12 / §17.14: same-class int pair, with a `+1` step so the chain has
// real arithmetic (the fold would otherwise short-circuit at the typecheck
// stage). The pre-fix shape is also SIGSEGV.
BEZEL_TEST(cyclic_class_const_initializer_4865,
           same_class_int_pair_rejected) {
    auto r = runAot(
        "module M; "
        "class A "
        "public static const I1: int := I2 + 1; "
        "public static const I2: int := I1 + 1; "
        "end A; "
        "class C "
        "public static method Main(): int "
        "Console.WriteLine(A.I1); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "cyclic class-level const initializer"));
}

// §22.12 / §17.14: cross-class pair (`A.K := B.K; B.K := A.K`). Same
// pre-fix shape: SIGSEGV in codegen.
BEZEL_TEST(cyclic_class_const_initializer_4865,
           cross_class_pair_rejected) {
    auto r = runAot(
        "module M; "
        "class A "
        "public static const K: int := B.K + 1; "
        "end A; "
        "class B "
        "public static const K: int := A.K + 1; "
        "end B; "
        "class C "
        "public static method Main(): int "
        "Console.WriteLine(A.K); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "cyclic class-level const initializer"));
}

// §22.12 / §17.14: a length-3 same-class chain (`K1 -> K2 -> K3 -> K1`).
// All three consts are LOCAL with AST ConstDecls; the graph has edges
// K1->K2, K2->K3, K3->K1, the DFS finds the back-edge and reports.
BEZEL_TEST(cyclic_class_const_initializer_4865,
           same_class_three_const_chain_rejected) {
    auto r = runAot(
        "module M; "
        "class A "
        "public static const K1: int := K2 + 1; "
        "public static const K2: int := K3 + 1; "
        "public static const K3: int := K1 + 1; "
        "end A; "
        "class C "
        "public static method Main(): int "
        "Console.WriteLine(A.K1); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "cyclic class-level const initializer"));
}

// §22.12: a single self-referential const (`K := K`). The graph has the
// single node K with a self-edge; the DFS in `color[K]==1` -> `K` reports.
// This is the cycle that folds the bare-identifier path of §17.14 cannot
// terminate on (depth-bound always increments, never bottoming out).
BEZEL_TEST(cyclic_class_const_initializer_4865,
           single_self_referential_const_rejected) {
    auto r = runAot(
        "module M; "
        "class A "
        "public static const K: int := K; "
        "end A; "
        "class C "
        "public static method Main(): int "
        "Console.WriteLine(A.K); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "cyclic class-level const initializer"));
}

// §22.12 / §17.14: UNUSED pair — the cycle pair declared but never
// referenced. Pre-fix: compiles cleanly with NO diagnostic (exit 0) because
// codegen never lowers the const. Post-fix: the sema pass rejects the
// cycle regardless of USE — the cycle is ill-formed at declaration time.
BEZEL_TEST(cyclic_class_const_initializer_4865,
           unused_pair_rejected) {
    auto r = runAot(
        "module M; "
        "class A "
        "public static const K1: double := K2; "
        "public static const K2: double := K1; "
        "end A; "
        "class C "
        "public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "cyclic class-level const initializer"));
}

// §22.12 / §17.14: NEGATIVE — a forward reference that is NOT a cycle.
// `K1 := K2 + 1; K2 := 7` — K2 has no reference to K1, so the graph has
// K1 -> K2 (only), and K2 is well-founded. Must compile cleanly and
// produce the value 8 at run time. Pins the no-false-reject half of the
// pass.
BEZEL_TEST(cyclic_class_const_initializer_4865,
           forward_reference_no_cycle_accepted) {
    auto r = runAot(
        "module M; "
        "class A "
        "public static const K2: int := 7; "
        "public static const K1: int := K2 + 1; "
        "end A; "
        "class C "
        "public static method Main(): int "
        "Console.WriteLine(A.K1); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8\n"));
}

// §22.12 / §17.14: NEGATIVE — a non-cyclic chain through MULTIPLE consts.
// `K1 := K2; K2 := K3; K3 := 5`. Each const references the NEXT; only K3
// is well-founded. The graph has K1 -> K2 -> K3, no back-edge. Must
// compile and run (K1 = 5).
BEZEL_TEST(cyclic_class_const_initializer_4865,
           forward_chain_through_three_consts_accepted) {
    auto r = runAot(
        "module M; "
        "class A "
        "public static const K3: int := 5; "
        "public static const K2: int := K3; "
        "public static const K1: int := K2; "
        "end A; "
        "class C "
        "public static method Main(): int "
        "Console.WriteLine(A.K1); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// §22.12: NEGATIVE — a cross-class non-cycle (`A.K := B.K; B.K := 7`).
// Only A references B; B has no edge back to A. Must compile.
BEZEL_TEST(cyclic_class_const_initializer_4865,
           cross_class_forward_only_accepted) {
    auto r = runAot(
        "module M; "
        "class A "
        "public static const K: int := B.K + 1; "
        "end A; "
        "class B "
        "public static const K: int := 7; "
        "end B; "
        "class C "
        "public static method Main(): int "
        "Console.WriteLine(A.K); "
        "return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8\n"));
}
