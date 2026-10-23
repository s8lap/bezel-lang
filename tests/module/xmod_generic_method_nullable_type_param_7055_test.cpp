// Regression test for issue #7055 (§14.1, §2.8.2, v0.340) and its fix-forward
// (#7078: the name-keyed type-param recognizer lacked the built-in-name guard).
//
// An importing module calls an imported generic METHOD at `T = int` by binding
// it to the defining module's erased-placeholder template body (§2.8.2). The
// template's ABI is the placeholder lowering: a type-parameter reference is
// the `int` (i32) placeholder, and its NULLABLE form `T?` is the placeholder
// nullable struct {i8 hasValue; i32 value}.
//
// The importer declared the imported method with `lowerTypeRef`, which drops
// the nullability of an IMPORTED type-parameter reference (it carries no
// `ast::Type` node, so the template rule in `lowerTypeRef` cannot fire) and
// answered a bare i32 for `T?`. So the importer's declaration of
// `N<T>(x: T): T?` was `i32 (i32)` against the exporter's `{i8, i32} (i32)`:
//   * the call read the exporter's {i8, i32} result as an int — the struct's
//     FIRST WORD, i.e. the hasValue tag plus padding — a garbage value
//     (MEASURED on the base: 1537927169 instead of 14), while the SEMANTIC
//     type of the expression stayed `int?`;
//   * the `??`, which keys on that semantic type — extracted field 0 of an
//     i32, and bezelc CRASHED (SIGSEGV in `llvm::Value::setName`, from an
//     `extractvalue` whose aggregate type has no result type).
//
// §14.1: a call at explicit type arguments is an instantiation site. §2.8.2
// clause 2: "An external instantiation behaves identically to a same-module
// instantiation of the same generic at the same arguments", and "An
// implementation unable to carry some body shape in its interface format must
// reject the export, or the import, with a diagnostic naming the shape; it
// must not alter the instantiation's semantics."
//
// The parameter side is the same ABI, so the same fix applies there: a `T?`
// parameter is declared as the placeholder nullable struct. A NULLABLE
// argument now crosses the boundary correctly (it crashed before); a bare
// `int` argument cannot be carried by the importer yet (it would have to be
// widened to `int?` at the call site — the importer-side generic-method
// monomorphization blocker of #1943 / #428 / #1937), so it is REJECTED with a
// located diagnostic instead of silently crossing at the wrong shape.
//
// ── THE COLLISION CLASS (fix-forward, #7078) ─────────────────────────────────
//
// A type parameter may be spelled like a reserved built-in: §14.1 "Type-parameter
// names" admits a type parameter named like an ordinary type, and §2.7 rule 1
// finds the type parameter first. So `M<IntPtr>(x: IntPtr): IntPtr?` declares a
// type parameter named `IntPtr`.
//
// The defining module's template lowers such a reference to the BUILT-IN's own
// layout — `lowerTypeRef`'s built-in arms answer before the type-parameter
// fallback reaches the i32 placeholder — so the importer's declaration must
// lower it the SAME way. The predecessor's recognizer matched the name alone
// and answered the placeholder {i8, i32} against the exporter's {i8, i64} (or
// i64): the two modules disagreed on the ABI of one symbol, and the call read
// a SILENT wrong value (0 instead of 14) — §2.8.2 clause 2's own acceptance
// criterion, "behaves identically to a same-module instantiation", unmet.
//
// The rows below pin the observable for EVERY built-in name in that collision
// class: the cross-module answer must equal the same-module answer.
//
// Each run row uses `runAotModulesChecked` (generated-code ASan + LSan,
// exitcode 99 on a leak) and asserts the printed value.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;

namespace {

const char* kLib7055 =
    "module Lib7055;\n"
    "public choice Opt<T> Some(v: T), No; end Opt;\n"
    "public class Api\n"
    "  public static method N<T>(x: T): T? where T: value => x;\n"
    "  public static method Take<T>(x: T?): T? where T: value => x;\n"
    "  public static method Plain<T>(x: T): T where T: value => x;\n"
    "end Api;\n"
    "public class Other\n"
    "  public static method M(x: int): int? => x;\n"
    "end Other;\n";

// A generic method whose OWN type parameter is named after a reserved built-in.
// §14.1 "Type-parameter names" admits it; §2.7 rule 1 makes the name denote
// the type parameter inside the method's scope.
std::string libWithTypeParamNamed(const std::string& typeParamName,
                                  const std::string& paramType,
                                  const std::string& returnType,
                                  const std::string& body = "x") {
    return "module LibNamed;\n"
           "public class Api\n"
           "  public static method M<" + typeParamName + ">(x: " + paramType +
           "): " + returnType + " where " + typeParamName +
           ": value => " + body + ";\n"
           "end Api;\n";
}

// The same declaration, in the module that CALLS it (the same-module control).
std::string sameModuleWithTypeParamNamed(const std::string& typeParamName,
                                          const std::string& paramType,
                                          const std::string& returnType) {
    return libWithTypeParamNamed(typeParamName, paramType, returnType) +
           "public class Program\n"
           "  public static method Main()\n"
           "    Console.WriteLine(Api.M<int>(14) ?? -1);\n"
           "  end Main;\n"
           "end Program;\n";
}

std::string app7055(const std::string& body) {
    return "module App7055;\n"
           "import Lib7055;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    " + body + "\n"
           "  end Main;\n"
           "end Program;\n";
}

std::string appNamed(const std::string& body) {
    return "module AppNamed;\n"
           "import LibNamed;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    " + body + "\n"
           "  end Main;\n"
           "end Program;\n";
}

void expectPrints(const std::string& body, const char* want) {
    JitResult r = bezel::test::runAotModulesChecked(
        {ModuleSource{"Lib7055", kLib7055},
         ModuleSource{"App7055", app7055(body)}},
        "App7055");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

// The cross-module call of a generic method whose type parameter is named
// after a built-in: the answer must be the SAME as the same-module answer.
void expectNamedPrints(const std::string& typeParamName,
                       const std::string& paramType,
                       const std::string& returnType,
                       const char* want) {
    JitResult r = bezel::test::runAotModulesChecked(
        {ModuleSource{"LibNamed",
                      libWithTypeParamNamed(typeParamName, paramType,
                                            returnType)},
         ModuleSource{"AppNamed", appNamed("Console.WriteLine(Api.M<int>(14) "
                                           "?? -1);")}},
        "AppNamed");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

// The same declaration called from the module that DEFINES it: the answer the
// cross-module call must reproduce (§2.8.2 clause 2).
void expectSameModuleNamedPrints(const std::string& typeParamName,
                                 const std::string& paramType,
                                 const std::string& returnType,
                                 const char* want) {
    JitResult r = bezel::test::runAotModulesChecked(
        {ModuleSource{"LibNamed",
                      sameModuleWithTypeParamNamed(typeParamName, paramType,
                                                  returnType)}},
        "LibNamed");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

// A cross-module generic call whose type argument does not match the
// compiled `int` (i32) placeholder: the importer-side monomorphization
// blocker (#1943 / #428 / #1937) rejects it with a LOCATED diagnostic —
// §2.8.2's "must reject the export, or the import, with a diagnostic naming
// the shape; it must not alter the instantiation's semantics".
void expectWidthRejected(const std::string& typeParamName,
                         const std::string& paramType,
                         const std::string& returnType,
                         const std::string& body = "x") {
    JitResult r = bezel::test::runAotModules(
        {ModuleSource{"LibNamed",
                      libWithTypeParamNamed(typeParamName, paramType,
                                            returnType, body)},
         ModuleSource{"AppNamed", appNamed("Console.WriteLine(Api.M<int>(14) "
                                           "?? -1);")}},
        "AppNamed");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "calling imported generic method `M`"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "the argument would cross the boundary at the wrong width"));
}

}  // namespace

// The issue's repro: an imported generic method whose return type is the
// nullable form of its own type parameter. Pre-fix: bezelc SEGFAULTED while
// compiling the importer (SIGSEGV in llvm::Value::setName).
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           issue_repro_nullable_type_param_return_prints_payload_spec_14_1_2_8_2) {
    expectPrints("var r := Api.N<int>(14); Console.WriteLine(r ?? -1);",
                 "14\n");
}

// The same value through the `??` in a larger expression.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           nullable_type_param_return_coalesced_in_expression_spec_14_1_2_8_2) {
    expectPrints("var r := Api.N<int>(14); Console.WriteLine((r ?? 0) + 1);",
                 "15\n");
}

// 200 iterations: the sum of payloads 0..199 is 19900 (pre-fix: a compiler
// crash), and LSan reports no leak.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           loop_200_sums_payloads_no_leak_spec_14_1_2_8_2) {
    expectPrints("var total := 0; "
                 "for var i in 0..<200 do "
                 "  total := total + (Api.N<int>(i) ?? -1); "
                 "end; "
                 "Console.WriteLine(total);",
                 "19900\n");
}

// The PARAMETER side of the same ABI: a nullable type parameter as a
// parameter. A NULLABLE argument crosses the boundary correctly now (pre-fix:
// the importer crashed on this too — the argument's {i8, i32} shape against
// the declared i32 parameter).
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           nullable_type_param_argument_crosses_boundary_spec_14_1_2_8_2) {
    expectPrints("var v: int? := 15; "
                 "Console.WriteLine(Api.Take<int>(v) ?? -1);",
                 "15\n");
}

// A bare `int` argument to a `T?` parameter cannot be carried across the
// boundary yet (the importer would have to widen it to `int?` at the call
// site — the #1943 / #428 / #1937 importer-side monomorphization blocker).
// Pre-fix this call was ACCEPTED and the argument crossed at the wrong shape
// (the callee read the int as a {i8, i32} struct); §2.8.2 clause 2 requires a
// located diagnostic instead of altered semantics, so the rejection is the
// pinned verdict.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           bare_int_argument_to_nullable_type_param_is_rejected_spec_2_8_2) {
    JitResult r = bezel::test::runAotModules(
        {ModuleSource{"Lib7055", kLib7055},
         ModuleSource{"App7055", app7055("Console.WriteLine(Api.Take<int>(14) "
                                         "?? -1);")}},
        "App7055");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "calling imported generic method `Take`"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "the argument would cross the boundary at the wrong width"));
}

// Control: a NON-nullable type parameter is unchanged — the placeholder is
// the bare i32 and the value crosses as it always did.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           non_nullable_type_param_return_unchanged_spec_14_1) {
    expectPrints("Console.WriteLine(Api.Plain<int>(14));", "14\n");
}

// Control: a NON-generic imported method with a nullable return is unchanged
// — its parameter/return types are not type-parameter references.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           non_generic_imported_nullable_return_unchanged_spec_2_8_2) {
    expectPrints("Console.WriteLine(Other.M(14) ?? -1);", "14\n");
}

// ── The collision class: a type parameter NAMED after a reserved built-in ──
//
// `M<IntPtr>(x: IntPtr): IntPtr?` — the type parameter's NAME is a built-in
// name. The importer's declaration and the exporter's definition must agree
// on the ABI (§2.8.2 clause 2); the answer must be the same-module answer.
//
// MEASURED on the rejected predecessor (79f7954f): the exporter defined
// `i64 (i64)` while the importer declared `{i8, i32} (i64)`, and the call
// printed 0 — a SILENT wrong value, where the same-module call prints 14.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_builtin_intptr_cross_module_prints_payload_spec_2_8_2) {
    expectNamedPrints("IntPtr", "IntPtr", "IntPtr?", "14\n");
}

// The same-module control for that row: the answer the cross-module call
// must reproduce (§2.8.2 clause 2, "behaves identically to a same-module
// instantiation of the same generic at the same arguments").
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_builtin_intptr_same_module_prints_payload_spec_2_8_2) {
    expectSameModuleNamedPrints("IntPtr", "IntPtr", "IntPtr?", "14\n");
}

// The same collision class, the other nullable VALUE built-in: `DateTime?` is
// the tagged {i8 hasValue; i64 value} struct (§7.2 / §2.5), so the two
// modules' declarations agree on {i8, i64} and the payload crosses whole.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_builtin_datetime_cross_module_prints_payload_spec_2_8_2) {
    expectNamedPrints("DateTime", "DateTime", "DateTime?", "14\n");
}

// `TimeSpan` is the same family as `DateTime` (§2.5: both a scalar int64 tick
// count), so it takes the same shape.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_builtin_timespan_cross_module_prints_payload_spec_2_8_2) {
    expectNamedPrints("TimeSpan", "TimeSpan", "TimeSpan?", "14\n");
}

// A NON-nullable built-in-named type parameter: the plain i64 scalar crosses
// unchanged (no tagged struct is involved), so the answer is the payload.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_builtin_intptr_plain_return_prints_payload_spec_2_8_2) {
    expectNamedPrints("IntPtr", "IntPtr", "IntPtr", "14\n");
}

// Control: a type parameter named after a USER type (not a reserved
// built-in) is the placeholder case the predecessor's recognizer was written
// for — unchanged, and NOT narrowed by the built-in-name guard.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_user_type_cross_module_prints_payload_spec_2_8_2) {
    expectNamedPrints("Widget", "Widget", "Widget?", "14\n");
}

// The remaining built-in families of the collision class: their type
// argument does not match the compiled `int` (i32) placeholder, so the
// importer rejects the call with a located diagnostic (§2.8.2's "must reject
// the export, or the import, with a diagnostic naming the shape"). The
// verdicts are pinned so a silent wrong value cannot come back.
//
// `Guid` — a 128-bit value struct (§2.5 / stdlib §2.6).
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_builtin_guid_cross_module_is_rejected_spec_2_8_2) {
    expectWidthRejected("Guid", "Guid", "Guid?");
}

// `Stopwatch` — a heap-pointer reference built-in (§2.5).
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_builtin_stopwatch_cross_module_is_rejected_spec_2_8_2) {
    expectWidthRejected("Stopwatch", "Stopwatch", "Stopwatch?");
}

// `Task` — a heap-pointer reference built-in used as a PARAMETER (a `Task`
// return is not admissible in this shape: §30.5's ownership transfer).
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_builtin_task_parameter_is_rejected_spec_2_8_2) {
    expectWidthRejected("Task", "Task", "int", "0");
}

// `IEnumerable` — a heap-pointer reference built-in used as a PARAMETER.
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_builtin_enumerable_parameter_is_rejected_spec_2_8_2) {
    expectWidthRejected("IEnumerable", "IEnumerable", "int", "0");
}

// `Span` — a value-struct built-in used as a PARAMETER (§4.14 / §4.15).
BEZEL_TEST(xmod_generic_method_nullable_type_param_7055,
           type_param_named_builtin_span_parameter_is_rejected_spec_2_8_2) {
    expectWidthRejected("Span", "Span", "int", "0");
}
