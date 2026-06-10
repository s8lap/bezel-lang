// Spec-derived tests for issue #591 — Console.Write / Console.WriteLine
// arity diagnostic (stdlib §2.1).
//
// stdlib §2.1 defines Console as:
//   public static method Write(value: string);
//   public static method Write(value: Object);
//   public static method WriteLine();
//   public static method WriteLine(value: string);
//   public static method WriteLine(value: Object);
//
// There is NO multi-argument composite-format overload.  A call with >1
// arguments had no matching overload in the stdlib; codegen's
// tryLowerConsoleCall returned false and the call was silently dropped
// (no output, no error).  This pass turns wrong-arity calls into a
// compile-time error (#591).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Returns true iff any diagnostic message mentions "Console." and the
// wrong-arity text ("no overload") — i.e. the #591 diagnostic fired.
bool has591Error(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("Console.") != std::string::npos &&
            d.message.find("no overload") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// --- Must-compile (regression): 1-arg Write/WriteLine still accepted ---

// stdlib §2.1 example: Console.WriteLine(value: string) — 1 arg, OK.
BEZEL_TEST(sema_console_arity_591, writeline_one_string_arg_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main() "
        "    Console.WriteLine(\"hello\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// stdlib §2.1: Console.Write(value: string) — 1 arg, OK.
BEZEL_TEST(sema_console_arity_591, write_one_string_arg_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main() "
        "    Console.Write(\"x\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// stdlib §2.1: Console.WriteLine() — 0 args, OK.
BEZEL_TEST(sema_console_arity_591, writeline_zero_args_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main() "
        "    Console.WriteLine(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// 1-arg Write with an int — Object overload, OK.
BEZEL_TEST(sema_console_arity_591, write_one_int_arg_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main() "
        "    Console.Write(42); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Must-fail: 2-argument call (#591 — the original silent trap) ---

// stdlib §2.1: Console.WriteLine("value={0}", 42) — 2 args, no overload.
// Before #591 this compiled silently and produced NO output at runtime.
BEZEL_TEST(sema_console_arity_591, writeline_two_args_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main() "
        "    Console.WriteLine(\"value={0}\", 42); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(has591Error(r));
}

// 3 args — also rejected (stdlib §2.1).
BEZEL_TEST(sema_console_arity_591, writeline_three_args_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main() "
        "    Console.WriteLine(\"{0}+{1}={2}\", 1, 2, 3); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(has591Error(r));
}

// Console.Write with 2 args — rejected (stdlib §2.1 has only Write(value)).
BEZEL_TEST(sema_console_arity_591, write_two_args_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main() "
        "    Console.Write(\"x={0}\", 99); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(has591Error(r));
}

// Console.Write with 0 args — rejected (Write always needs exactly 1 arg,
// stdlib §2.1 has no Write() overload).
BEZEL_TEST(sema_console_arity_591, write_zero_args_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main() "
        "    Console.Write(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(has591Error(r));
}

// --- Console.Error.Write/WriteLine also rejected at wrong arity ---

BEZEL_TEST(sema_console_arity_591, error_stream_two_args_diagnoses) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main() "
        "    Console.Error.WriteLine(\"err={0}\", 1); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(has591Error(r));
}
