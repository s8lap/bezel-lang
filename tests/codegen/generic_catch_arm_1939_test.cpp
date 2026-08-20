// Codegen tests for §31.2/§31.2.1/§12.7/§2.8.3 rule 4 (#1939 leg 3, spec v0.220):
// catch-arm dispatch for GENERIC exception types honors the type argument.
//
// §31.2.1: a bare-type arm "matches any thrown exception that is an instance
// of the type". Pre-fix the catch dispatch (LlvmBackend_Stmts.cpp) resolved an
// arm's descriptor via descriptorForTypeName only — the SHARED per-declaration
// descriptor — while the THROW site mints a per-monomorphization descriptor
// (#1843, specializedVtableFor slot 0) whose chain does NOT contain the shared
// one. Net effect on dd33abd (probe /var/tmp/bezel-scratch/laneE/
// p1939_catch.bzl): a thrown `MyExc<string>` matched NEITHER `MyExc<int>` NOR
// `MyExc<string>` and fell through to the bare `Exception` arm (exit 3 — a
// FALSE NEGATIVE / wrong-arm selection; #1939's originally-predicted false
// positive evolved into this after #1843).
//
// The fix routes catch-arm targets through perInstDescriptorForTarget (the
// same resolution `is`/`as` and case patterns use), and enriches the SHARED
// descriptor chain of a type with a CONCRETE constructed generic base
// (`class SubExc : MyExc<string>`) with the per-instantiation base descriptors
// (instanceDescriptorFor), so subtype matching (§31.2.1 "subtypes included")
// works through a concrete generic base — and the same enrichment fixes the
// plain `is` false negative on such derived classes.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// A generic exception, a subclass pinned at a concrete instantiation, and a
// Main that throws `thrownExpr` and reports which arm caught it:
//   arm 1: MyExc<int> → 1;  arm 2: MyExc<string> → 2;  arm 3: Exception → 3.
std::string program(const char* thrownExpr) {
    return std::string(
        "module M; "
        "class MyExc<T>: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end MyExc; "
        "class SubExc: MyExc<string> "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end SubExc; "
        "class C public static method Main(): int "
        "  try "
        "    throw ") + thrownExpr + "; "
        "  catch "
        "    var e1: MyExc<int> => do return 1; end, "
        "    var e2: MyExc<string> => do return 2; end, "
        "    var e3: Exception => do return 3; end "
        "  end; "
        "  return 0; "
        "end Main; end C;";
}

}  // namespace

// (1) EXACT match: thrown MyExc<string> must select the MyExc<string> arm.
// FAILS WITHOUT the fix (returns 3 — fell through to Exception, the verified
// false negative).
BEZEL_TEST(generic_catch_arm_1939, exact_instantiation_selected_31_2_1) {
    BEZEL_EXPECT_EQ(run(program("new MyExc<string>(\"boom\")")), 2);
}

// (2) SIBLING precision: thrown MyExc<int> selects MyExc<int> — and NOT the
// MyExc<string> arm (the §12.9-invariant no-false-positive side).
BEZEL_TEST(generic_catch_arm_1939, sibling_instantiation_not_matched_12_9) {
    BEZEL_EXPECT_EQ(run(program("new MyExc<int>(\"boom\")")), 1);
}

// (3) SUBTYPE through a CONCRETE generic base (§31.2.1 "subtypes included"):
// thrown SubExc (: MyExc<string>) must select the MyExc<string> arm — needs
// the shared-chain enrichment (SubExc's chain lists the per-inst MyExc<string>
// descriptor).
BEZEL_TEST(generic_catch_arm_1939, subtype_via_concrete_generic_base_31_2_1) {
    BEZEL_EXPECT_EQ(run(program("new SubExc(\"boom\")")), 2);
}

// (4) FALLBACK stays sound: thrown MyExc<string> with only a MyExc<int> arm
// before Exception falls to Exception (no false positive on the sibling arm).
BEZEL_TEST(generic_catch_arm_1939, no_sibling_false_positive_12_9) {
    int rc = run(
        "module M; "
        "class MyExc<T>: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end MyExc; "
        "class C public static method Main(): int "
        "  try "
        "    throw new MyExc<string>(\"boom\"); "
        "  catch "
        "    var e1: MyExc<int> => do return 1; end, "
        "    var e2: Exception => do return 3; end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 3);
}

// (5) The same chain enrichment fixes the plain `is`/`as` FALSE NEGATIVE on a
// non-generic class deriving a CONCRETE generic base: a `SubK` value IS a
// `KBase<string>` (§12.7) and is NOT a `KBase<int>` (§12.9). Expect 10.
BEZEL_TEST(generic_catch_arm_1939, derived_class_is_concrete_generic_base_12_7) {
    int rc = run(
        "module M; "
        "class KBase<T> public constructor() end constructor; end KBase; "
        "class SubK: KBase<string> "
        "  public constructor() base(); end constructor; "
        "end SubK; "
        "class C public static method Main(): int "
        "  using var d := new SubK(); "
        "  var o: Object := d; "
        "  var r1 := (if o is KBase<string> then 1 else 0); "
        "  var r2 := (if o is KBase<int> then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}

// (5b) The interface cell of the same enrichment: a non-generic CLASS
// implementing a CONCRETE generic interface — `is IM<int>` on it must be true
// (§12.7) and `is IM<string>` false (§12.9). Expect 10.
BEZEL_TEST(generic_catch_arm_1939, class_implements_concrete_generic_iface_12_7) {
    int rc = run(
        "module M; "
        "interface IM<T> end IM; "
        "class Impl: IM<int> public constructor() end constructor; end Impl; "
        "class C public static method Main(): int "
        "  using var v := new Impl(); "
        "  var o: Object := v; "
        "  var r1 := (if o is IM<int> then 1 else 0); "
        "  var r2 := (if o is IM<string> then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}

// (6) Generic-body arm (§31.2.1 type-parameter matching, the #1505 family):
// `catch var e: MyExc<T>` inside a monomorphized body matches the SUBSTITUTED
// instantiation — T=string catches MyExc<string> (arm 1) and lets MyExc<int>
// fall through (arm 2). perInstDescriptorForTarget resolves the active
// substitution. Expect 12.
BEZEL_TEST(generic_catch_arm_1939, tparam_constructed_arm_substituted_31_2_1) {
    int rc = run(
        "module M; "
        "class MyExc<T>: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end MyExc; "
        "static class H "
        "  public static method Probe<T>(action: () -> ()): int "
        "    try "
        "      action(); "
        "    catch "
        "      var e1: MyExc<T> => do return 1; end, "
        "      var e2: Exception => do return 2; end "
        "    end; "
        "    return 0; "
        "  end Probe; "
        "end H; "
        "class C public static method Main(): int "
        "  var a := H.Probe<string>(lambda() => do "
        "    throw new MyExc<string>(\"a\"); end); "
        "  var b := H.Probe<string>(lambda() => do "
        "    throw new MyExc<int>(\"b\"); end); "
        "  return a * 10 + b; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 12);
}
