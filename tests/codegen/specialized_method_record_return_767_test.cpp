// fix #767 / §14.1: a generic method whose declared return type is a bare type
// parameter `T` bound to a RECORD lowers without an IR return-type mismatch.
//
// For `Make<T>(): T where T: new()` called as `Make<R>` (R a record), the
// specialized function's return type was computed as lowerTypeRef(T), which
// yielded the cached UNSPECIALIZED record struct from genericSubst_ — while the
// body's `new T()` produced the per-instantiation SPECIALIZED struct. The
// mismatch failed IR verification ("Function return type does not match operand
// type of return inst"). The fix re-derives the return type via recordStruct
// under the active substitution (restricted to a bare, non-nullable type-param
// return so nullable `T?` and direct record returns are unaffected).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §14.1: the #767 repro — `Make<R>()` returning `new T()` compiles (was IR_FAIL)
// and the returned record's field reads back (the default int, 0).
BEZEL_TEST(specialized_method_record_return_767, returns_record_type_param) {
    BEZEL_EXPECT_EQ(runFull(
        "module TestGen; "
        "record R(X: int); "
        "static class F "
        "  public static method Make<T>(): T where T: new() "
        "    return new T(); "
        "  end Make; "
        "end F; "
        "public static class Program "
        "  public static method Main(): int "
        "    var r: R := F.Make<R>(); "
        "    return r.X; "  // default int → 0
        "  end Main; "
        "end Program;"), 0);
}

// §14.2: two distinct record instantiations of the same generic method each get
// their own specialized return type (no struct collision). R has one int field
// (read 0); R2 has two (read 0).
BEZEL_TEST(specialized_method_record_return_767, two_record_instantiations) {
    BEZEL_EXPECT_EQ(runFull(
        "module TestGen; "
        "record R(X: int); "
        "record R2(A: int, B: int); "
        "static class F "
        "  public static method Make<T>(): T where T: new() return new T(); end Make; "
        "end F; "
        "public static class Program "
        "  public static method Main(): int "
        "    var r: R := F.Make<R>(); "
        "    var r2: R2 := F.Make<R2>(); "
        "    return r.X + r2.A + r2.B; "  // 0
        "  end Main; "
        "end Program;"), 0);
}
