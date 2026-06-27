// Regression tests for #1107 / §30.5 (sweep #9, finding #10): `Task.FromResult<T>`
// is restricted to NON-class T — value types, `string`, runtime-managed
// reference types. A class (or interface) T must use `Task.FromOwnedResult<T>`
// (Pattern B: the produced task owns the result, transferred to the awaiter on
// `using var x := await …`). The spec (§30.5, L6357/L6558) makes a class-typed T
// a compile error; the compiler silently accepted it (no diagnostic). Sema now
// rejects a class-typed T — taken from the explicit type argument or inferred
// from the value argument — pointing to the owned variant.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
bool has305Reject(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("\xC2\xA7""30.5") != std::string::npos ||
            d.message.find("FromOwnedResult") != std::string::npos)
            return true;
    return false;
}
}  // namespace

// #1107/#10: explicit class type arg `Task.FromResult<MyClass>(…)` → rejected.
BEZEL_TEST(task_fromresult_class_reject_1107, explicit_class_type_arg_rejected) {
    auto r = runJit(
        "module M; class MyC public _n: int; destructor end destructor; end MyC; "
        "class C public static async method Run(): Task<int> "
        "  using var t := Task.FromResult<MyC>(new MyC()); return 0; "
        "end Run; public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has305Reject(r));
}

// #1107/#10: inferred class T (from the argument) → rejected.
BEZEL_TEST(task_fromresult_class_reject_1107, inferred_class_arg_rejected) {
    auto r = runJit(
        "module M; class MyC public _n: int; destructor end destructor; end MyC; "
        "class C public static async method Run(): Task<int> "
        "  var c := new MyC(); var t := Task.FromResult(c); return 0; "
        "end Run; public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has305Reject(r));
}

// Regression guard: a value-typed T (int) is accepted and awaitable.
BEZEL_TEST(task_fromresult_class_reject_1107, int_value_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Run(): Task<int> "
        "    var t := Task.FromResult(42); return await t; "          // 42
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    return await C.Run(); "
        "  end Main; end C;"), 42);
}

// Regression guard: `string` (runtime-managed) is accepted.
BEZEL_TEST(task_fromresult_class_reject_1107, string_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Run(): Task<int> "
        "    var t := Task.FromResult(\"hi\"); return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; end C;"), 0);
}

// Regression guard: a record (value type) is accepted.
BEZEL_TEST(task_fromresult_class_reject_1107, record_value_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(X: int); class C "
        "  public static async method Run(): Task<int> "
        "    var t := Task.FromResult(new Pt(5)); return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; end C;"), 0);
}
