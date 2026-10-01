// fix #801 / §19.3 / §19.5 / §9.6 / §10: a `case` with a TYPED binding pattern
// (`var q: T`) over an UNBOXED record (or enum) scrutinee crashed the compiler —
// `patternMatches` emitted a runtime is-a test (`emitIsA`, which walks a vtable
// through a pointer) for a struct/i32 VALUE, producing an ICmp on mismatched
// operand types (LLVM assertion / IR-verify failure).
//
// A record/enum is a sealed value type with no subtyping, so a typed binding over
// an unboxed such scrutinee is a STATIC test (the scrutinee's static type either
// IS the pattern type or is not). A BOXED record/enum (an `Object`/interface
// scrutinee — a pointer) remains a genuine runtime is-a (the §19.5 unbox case).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/aot_result_asserts.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §19: a typed binding whose pattern type IS the (unboxed) record scrutinee's
// type matches and binds the value. Previously a compiler crash.
BEZEL_TEST(case_record_typed_binding_801, record_same_type_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(40, 2); "
        "  return (case p of var q: Pt => q.x + q.y, _ => 0); "  // 42
        "end Main; end C;"), 42);
}

// §19.3 / Appendix C.1 (v0.313, #5188) FLIP (was: never matches, falls through
// to the wildcard, exit 99): a typed binding of a DIFFERENT record type over an
// unboxed record can never succeed and is now a compile error.
BEZEL_TEST(case_record_typed_binding_801, record_different_type_rejected_5188) {
    auto r = runAot(
        "module M; record Pt(x: int, y: int); record Qt(a: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(40, 2); "
        "  return (case p of var q: Qt => q.a, _ => 99); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "naming a different value type"));
}

// §10: a typed binding over an unboxed enum scrutinee (an i32 value) — static
// match, no crash.
BEZEL_TEST(case_record_typed_binding_801, enum_typed_binding_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum E A, B, C; end E; "
        "class C public static method Main(): int "
        "  var e := E.B; "
        "  return (case e of var q: E => 42, _ => 0); "  // 42
        "end Main; end C;"), 42);
}

// §19.5 regression: a typed binding over a BOXED record (`Object?` scrutinee — a
// pointer) is still a RUNTIME is-a test (unbox). Pins that the static-test fix
// did not disturb the boxed path.
BEZEL_TEST(case_record_typed_binding_801, boxed_record_binding_runtime_isa) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(40, 2); "
        "  var o: Object? := p; "
        "  return (case o of var q: Pt => q.x + q.y, _ => 0); "  // 42
        "end Main; end C;"), 42);
}
