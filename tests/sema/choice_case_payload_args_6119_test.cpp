// fix #6119 / §35.3 (v0.338): "Payload arguments bind to the case's payload
// fields as to a parameter list" — a payload-carrying case construction
// `Type.Case(args)` gets the §22.5 binding (arity, names) and the §5.1
// per-argument conversion check of an ordinary call. `typeCall` typed the
// construction but never checked its arguments, so `CB.One(a)` with `a: A` for
// `One(v: B)` compiled and the payload was read as a `B` (ASan
// heap-buffer-overflow); a string for an `int` payload, a wrong argument
// count, and a `null` for a non-null payload were all accepted likewise.
//
// Rows: R* = rejected with a located diagnostic naming the case; C* = the
// valid spellings, run with stdout asserted.
//
// Not pinned here (pre-existing, other defects): `Object` -> `B` is the
// shared `checkAssignable` Object-source arm (#6023); `int` -> `long` and a
// widened payload at a generic instantiation fail in codegen; an `int` for a
// `[N]int` payload is accepted by `checkAssignable` for ordinary calls too.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
const char* kPre = R"(module M6119;
public class A
  public constructor() end constructor;
  public x: int := 1;
end A;
public class B: A
  public constructor() end constructor;
  public y: int := 9;
end B;
public record R(a: int);
public choice N1 One(i: int); end N1;
)";
std::string prog(const std::string& decls, const std::string& body) {
    return std::string(kPre) + decls +
           "\npublic class P\n  public static method Show(n: N1): int\n"
           "    case n of One(var i) => Console.WriteLine(i) end case;\n"
           "    return 0;\n  end Show;\n"
           "  public static method Main(): int\n" + body +
           "\n    return 0;\n  end Main;\nend P;\n";
}
}  // namespace

// R1
BEZEL_TEST(sema_choice_case_payload_args_6119, downcast_class_to_derived_payload_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: B), Two; end C;",
        "    var a := new A(); var c := C.One(a);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "to case `C.One`"));
}

// R2
BEZEL_TEST(sema_choice_case_payload_args_6119, string_for_int_payload_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int), Two; end C;",
        "    var c := C.One(\"x\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "to case `C.One`"));
}

// R3
BEZEL_TEST(sema_choice_case_payload_args_6119, int_for_string_payload_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: string), Two; end C;",
        "    var c := C.One(5);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "to case `C.One`"));
}

// R4
BEZEL_TEST(sema_choice_case_payload_args_6119, long_for_int_payload_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int), Two; end C;",
        "    var i: long := 5; var c := C.One(i);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "to case `C.One`"));
}

// R5
BEZEL_TEST(sema_choice_case_payload_args_6119, null_for_nonnull_class_payload_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: A), Two; end C;",
        "    var c := C.One(null);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is nullable but the target type is non-nullable"));
}

// R6
BEZEL_TEST(sema_choice_case_payload_args_6119, null_for_int_payload_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int), Two; end C;",
        "    var c := C.One(null);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is nullable but the target type is non-nullable"));
}

// R7
BEZEL_TEST(sema_choice_case_payload_args_6119, nullable_for_nonnull_payload_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int), Two; end C;",
        "    var i: int? := 5; var c := C.One(i);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is nullable but the target type is non-nullable"));
}

// R8
BEZEL_TEST(sema_choice_case_payload_args_6119, second_payload_mismatch_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int, w: string), Two; end C;",
        "    var c := C.One(5, 6);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "argument `w` to case `C.One`"));
}

// R9
BEZEL_TEST(sema_choice_case_payload_args_6119, first_payload_mismatch_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int, w: string), Two; end C;",
        "    var c := C.One(\"a\", \"b\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "argument `v` to case `C.One`"));
}

// R10
BEZEL_TEST(sema_choice_case_payload_args_6119, named_payload_mismatch_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int, w: string), Two; end C;",
        "    var c := C.One(w := 1, v := 4);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "argument `w` to case `C.One`"));
}

// R11
BEZEL_TEST(sema_choice_case_payload_args_6119, function_payload_mismatch_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: (int) -> int), Two; end C;",
        "    var c := C.One(5);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "to case `C.One`"));
}

// R12
BEZEL_TEST(sema_choice_case_payload_args_6119, nested_choice_payload_mismatch_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: N1), Two; end C;",
        "    var c := C.One(5);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "to case `C.One`"));
}

// R13
BEZEL_TEST(sema_choice_case_payload_args_6119, record_payload_mismatch_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: R), Two; end C;",
        "    var c := C.One(5);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "to case `C.One`"));
}

// R14
BEZEL_TEST(sema_choice_case_payload_args_6119, generic_payload_mismatch_rejected_35_3_14_1) {
    auto r = runAot(prog(
        "public choice G<T> Some(v: T), None; end G;",
        "    var c := G<int>.Some(\"s\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "to case `G.Some`"));
}

// R15
BEZEL_TEST(sema_choice_case_payload_args_6119, extra_argument_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int), Two; end C;",
        "    var c := C.One(1, 2);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "supplies 2 argument(s) but the parameter list has 1"));
}

// R16
BEZEL_TEST(sema_choice_case_payload_args_6119, missing_argument_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int, w: int), Two; end C;",
        "    var c := C.One(1);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "required parameter `w` is not supplied"));
}

// R17
BEZEL_TEST(sema_choice_case_payload_args_6119, payload_free_case_with_argument_rejected_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int), Two; end C;",
        "    var c := C.Two(1);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "supplies 1 argument(s) but the parameter list has 0"));
}

// R17 — the same check in an argument position (`Show(N1.One("s"))`).
BEZEL_TEST(sema_choice_case_payload_args_6119,
           argument_position_mismatch_rejected_35_3) {
    auto r = runAot(prog("", "    Show(N1.One(\"s\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "to case `N1.One`"));
}

// C1
BEZEL_TEST(sema_choice_case_payload_args_6119, exact_int_payload_runs_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int), Two; end C;",
        "    var c := C.One(5); case c of One(var v) => Console.WriteLine(v), Two => Console.WriteLine(0) end case;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// C2
BEZEL_TEST(sema_choice_case_payload_args_6119, int_into_nullable_payload_runs_35_3_5_1) {
    auto r = runAot(prog(
        "public choice C One(v: int?), Two; end C;",
        "    var c := C.One(5); case c of One(var v) => Console.WriteLine(v), Two => Console.WriteLine(0) end case;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// C3
BEZEL_TEST(sema_choice_case_payload_args_6119, derived_into_base_payload_runs_35_3_5_1) {
    auto r = runAot(prog(
        "public choice C One(v: A), Two; end C;",
        "    var b := new B(); var c := C.One(b); case c of One(var v) => Console.WriteLine(v.x), Two => Console.WriteLine(0) end case;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// C4
BEZEL_TEST(sema_choice_case_payload_args_6119, two_payloads_run_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int, w: string), Two; end C;",
        "    var c := C.One(5, \"s\"); case c of One(var v, var w) => Console.WriteLine($\"{v}{w}\"), Two => Console.WriteLine(0) end case;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5s\n"));
}

// C5
BEZEL_TEST(sema_choice_case_payload_args_6119, named_payloads_reordered_run_35_3_22_5) {
    auto r = runAot(prog(
        "public choice C One(v: int, w: string), Two; end C;",
        "    var c := C.One(w := \"z\", v := 4); case c of One(var v, var w) => Console.WriteLine($\"{v}{w}\"), Two => Console.WriteLine(0) end case;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4z\n"));
}

// C6
BEZEL_TEST(sema_choice_case_payload_args_6119, function_payload_runs_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: (int) -> int), Two; end C;\npublic class H public static method Dbl(x: int): int => x * 2; end H;",
        "    var c := C.One(H.Dbl); case c of One(var f) => Console.WriteLine(f(4)), Two => Console.WriteLine(0) end case;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8\n"));
}

// C7
BEZEL_TEST(sema_choice_case_payload_args_6119, nested_choice_payload_runs_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: N1), Two; end C;",
        "    var c := C.One(N1.One(7)); case c of One(var n) => Show(n), Two => Console.WriteLine(0) end case;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// C8
BEZEL_TEST(sema_choice_case_payload_args_6119, generic_choice_instantiation_runs_35_3_14_1) {
    auto r = runAot(prog(
        "public choice G<T> Some(v: T), None; end G;",
        "    var c := G<int>.Some(7); case c of Some(var v) => Console.WriteLine(v), None => Console.WriteLine(0) end case;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// C9
BEZEL_TEST(sema_choice_case_payload_args_6119, payload_free_case_runs_35_3) {
    auto r = runAot(prog(
        "public choice C One(v: int), Two; end C;",
        "    var c := C.Two; case c of One(var v) => Console.WriteLine(v), Two => Console.WriteLine(2) end case;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}
