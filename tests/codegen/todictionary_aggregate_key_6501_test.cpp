// Regression tests for #6501 (§3.6, §8.3, stdlib §5, v0.338): `ToDictionary`
// keyed by an INLINE ARRAY aborted the compiler. `buildLinqTrampoline` kind 2
// (the key trampoline) ends in `CreateSExt(k, i64())` for any key that is
// neither a pointer nor i64, and an inline-array key lowers to `[N x T]`:
//
//   SExt only operates on integer
//     %3 = sext [2 x i32] %cl.ret to i64
//   bezelc: codegen error: IR verification failed
//
// The key crosses the runtime boundary as the trampoline's single `int64_t`
// return and `bz_lazy_to_dict` copies at most 8 bytes of it, so an aggregate key
// is unrepresentable by ABI; the record / `decimal` / `float` / `double` keys are
// already refused with a located diagnostic before codegen. The inline array now
// takes that same refusal instead of an abort. Rows are enumerated over the key
// type: the aggregate and floating-point keys must be REJECTED with the
// diagnostic (and not crash), the scalar / reference keys must keep RUNNING
// (stdout asserted), so the guard is pinned from both sides.
//
// The positive "inline-array key works" shape is tracked separately.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// `Array<E>.ToDictionary(lambda(x: E) => KEYEXPR)` over two elements.
std::string prog(const std::string& decls, const std::string& elemTy,
                 const std::string& elems, const std::string& keyTy,
                 const std::string& keyExpr) {
    return "module M;\n" + decls +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<" + elemTy + "> := " + elems + ";\n"
           "    var d := a.ToDictionary(lambda(x: " + elemTy + "): " + keyTy +
           " => " + keyExpr + ");\n"
           "    Console.WriteLine(d.Count);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

const char* kNeedle = "`ToDictionary`'s key selector currently supports only";

void expectRejected(const JitResult& r) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle));
}

}  // namespace

// The issue's shape: an inline-array key (bound locals, as #6271 notes).
BEZEL_TEST(todictionary_aggregate_key_6501, inline_array_key_is_refused_not_aborted) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var k1: [2]int := [1, 2];\n"
        "    var k2: [2]int := [3, 4];\n"
        "    var a: Array<[2]int> := [k1, k2];\n"
        "    var d := a.ToDictionary(lambda(x: [2]int) => x);\n"
        "    Console.WriteLine(d.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectRejected(r);
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(r, "IR verification failed"));
}

// An inline array selected out of a wider element (not the identity selector).
BEZEL_TEST(todictionary_aggregate_key_6501, inline_array_key_from_a_field_is_refused) {
    JitResult r = runAot(
        "module M;\n"
        "record P(Xs: [3]int)\n"
        "end P;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v: [3]int := [1, 2, 3];\n"
        "    var a: Array<P> := [new P(v)];\n"
        "    var d := a.ToDictionary(lambda(p: P) => p.Xs);\n"
        "    Console.WriteLine(d.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    expectRejected(r);
}

// The already-refused aggregate / floating-point keys stay refused.
BEZEL_TEST(todictionary_aggregate_key_6501, record_key_is_refused) {
    expectRejected(runAot(prog("record P(X: int)\nend P;\n", "int", "[1, 2]",
                               "P", "new P(x)")));
}

BEZEL_TEST(todictionary_aggregate_key_6501, decimal_key_is_refused) {
    expectRejected(runAot(prog("", "int", "[1, 2]", "decimal", "1.5m")));
}

BEZEL_TEST(todictionary_aggregate_key_6501, double_key_is_refused) {
    expectRejected(runAot(prog("", "int", "[1, 2]", "double", "1.5")));
}

// The accepted key types keep running (stdout), so the guard is not over-broad.
BEZEL_TEST(todictionary_aggregate_key_6501, scalar_and_reference_keys_still_run) {
    BEZEL_EXPECT_EQ(runAot(prog("", "int", "[1, 2]", "int", "x")).stdoutText,
                    std::string("2\n"));
    BEZEL_EXPECT_EQ(runAot(prog("", "int", "[1, 2]", "long", "x * 5000000000L")).stdoutText,
                    std::string("2\n"));
    BEZEL_EXPECT_EQ(runAot(prog("", "int", "[1, 2]", "string", "$\"k{x}\"")).stdoutText,
                    std::string("2\n"));
    BEZEL_EXPECT_EQ(runAot(prog("", "int", "[1, 2]", "bool", "x > 1")).stdoutText,
                    std::string("2\n"));
    BEZEL_EXPECT_EQ(runAot(prog("enum E\n  A, B;\nend E;\n", "int", "[1, 2]",
                                "E", "(if x = 1 then E.A else E.B)")).stdoutText,
                    std::string("2\n"));
}
