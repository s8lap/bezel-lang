// Regression tests for issue #4137 — the `?.` (null-conditional) sibling of
// `narrow #4120`'s `.`-form fix for CString/WString/RawBytes.
//
// `narrow #4120` gave the PLAIN `.` form of `.ToString()`/`.Equals()`/
// `.GetHashCode()` a correct §8.3/§8.5 Object-contract default for these
// three FFI-boundary buffer types, but deliberately left the `?.` sibling
// site (`ncoIsClass`, `Emitter::lowerNullCondCall`, LlvmBackend_Calls2.cpp)
// untouched: that function had no qualified-name/identity replacement
// dispatch to fall back to, so excluding the misdispatch there with no
// replacement risked the same "wrong value -> crash" regression #4120's own
// development measured and reverted for the fully general exclusion.
//
// Measured pre-fix (base 2aa5b1d, a genuinely non-null `CString?` obtained
// via `extern "c" strdup(...): owned CString?`): `c?.ToString()`,
// `c?.GetHashCode()`, `c?.Equals(c)` ALL degraded to `null` (rendered as the
// literal text "NULL"/"null" depending on the print path) via the
// pre-existing #2434 zero-initialized-slot fallback (a Warning, not
// silent). Root cause, precisely: for the ANNOTATION shape (`declaration ==
// nullptr`, the shape an extern return type or a `c: CString?` parameter
// produces), `ncoIsClass` was ALREADY false (it requires a non-null
// `declaration`), so the receiver matched NEITHER `ncoIsObj` nor `ncoIsIface`
// nor `ncoIsClass` and fell straight through to the function's `!ms` bail —
// not even reaching the (buggy) generic vtable dispatch the `.` form's
// pre-#4120 `cg_isClass` reached. For the FACTORY shape
// (`WString.From(...)`/`RawBytes.From(...)`, `declaration ==
// builtinClassSym(...)`, `kind == Class`), `ncoIsClass` WAS true and
// misdispatched exactly like the `.` form did pre-#4120.
//
// FIX: a new `ncoIsFfiBuffer` (`Emitter::isFfiBufferTypeRef`, the SAME
// dual-TypeRef-shape authority #4120 used for the `.` form) is admitted
// alongside `ncoIsObj`/`ncoIsIface`/`ncoIsClass`, and excluded FROM
// `ncoIsClass` as a backstop. Inside the shared null-guard/wrap/PHI
// machinery this function already has, an FFI-buffer receiver now computes
// its result via the qualified-name §8.5 literal (ToString) or the §8.3
// reference-identity default (Equals/GetHashCode, `bz_obj_identity_hash`/
// pointer compare) instead of dispatching through `lowerObjectToString`/
// `lowerObjectEquals`/`lowerObjectGetHashCode`'s vtable-slot jump.
//
// Cite: stdlib §10.1/§10.2/§10.3, language spec §7.5/§8.3/§8.5, v0.295.
// Related: #4120 (the `.` half, landed as `narrow`), #4126 (the sibling
// matrix of OTHER broken builtin cells), #2434 (the degrade path), #3907
// (the same defect class for FrozenArray).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ═══════════ ANNOTATION shape (declaration == nullptr) via extern ═════════
// The exact repro shape from issue #4137's own filing: a genuinely non-null
// `CString?` from an extern function's return type annotation.
BEZEL_TEST(ffi_buffer_nco_4137, cstring_extern_annotation_tostring_correct_value_spec_8_5) {
    const std::string p =
        "module M; "
        "extern \"c\" public method strdup(s: CString): owned CString?; end; "
        "class C public static method Main(): int "
        "  using var name := CString.From(\"hi\"); "
        "  using var copy := strdup(name); "
        "  var s := copy?.ToString(); "
        "  return (if s = \"System.CString\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(ffi_buffer_nco_4137, cstring_extern_annotation_gethashcode_correct_value_spec_8_3) {
    const std::string p =
        "module M; "
        "extern \"c\" public method strdup(s: CString): owned CString?; end; "
        "class C public static method Main(): int "
        "  using var name := CString.From(\"hi\"); "
        "  using var copy := strdup(name); "
        "  var h1 := copy?.GetHashCode(); "
        "  var h2 := copy?.GetHashCode(); "
        "  return (if h1 = h2 and h1 <> 0 then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(ffi_buffer_nco_4137, cstring_extern_annotation_equals_correct_value_spec_8_3) {
    const std::string p =
        "module M; "
        "extern \"c\" public method strdup(s: CString): owned CString?; end; "
        "class C public static method Main(): int "
        "  using var name := CString.From(\"hi\"); "
        "  using var copy := strdup(name); "
        "  var e := copy?.Equals(copy); "
        "  return (if e <> null and e! then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// ═══════════ FACTORY shape (declaration == builtinClassSym) ═══════════════
BEZEL_TEST(ffi_buffer_nco_4137, wstring_factory_shape_tostring_correct_value_spec_8_5) {
    const std::string p = prog(
        "  using var w: WString? := WString.From(\"hi\"); "
        "  var s := w?.ToString(); "
        "  return (if s = \"System.WString\" then 42 else 1); ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(ffi_buffer_nco_4137, rawbytes_factory_shape_tostring_correct_value_spec_8_5) {
    const std::string p = prog(
        "  var arr: Array<byte> := [byte(1), byte(2), byte(3)]; "
        "  using var rb: RawBytes? := RawBytes.From(arr); "
        "  var s := rb?.ToString(); "
        "  return (if s = \"System.RawBytes\" then 42 else 1); ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(ffi_buffer_nco_4137,
           rawbytes_factory_shape_equals_and_gethashcode_correct_value_spec_8_3) {
    const std::string p = prog(
        "  var arr: Array<byte> := [byte(1), byte(2), byte(3)]; "
        "  using var rb1: RawBytes? := RawBytes.From(arr); "
        "  using var rb2: RawBytes? := RawBytes.From(arr); "
        "  var selfEq := rb1?.Equals(rb1); "
        "  var crossEq := rb1?.Equals(rb2); "
        "  var h1a := rb1?.GetHashCode(); "
        "  var h1b := rb1?.GetHashCode(); "
        "  if selfEq = null or not selfEq! then return 1; end; "
        "  if crossEq = null or crossEq! then return 2; end; "  // distinct objects: NOT equal
        "  if h1a <> h1b then return 3; end; "
        "  return 42; ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// ═══════════ §7.5 null-receiver short-circuit is preserved ════════════════
// A genuinely NULL CString? must still short-circuit to null (the call is
// never dispatched at all), exactly as it did before this fix.
BEZEL_TEST(ffi_buffer_nco_4137, cstring_null_receiver_short_circuits_spec_7_5) {
    const std::string p = prog(
        "  var n: CString? := null; "
        "  var s := n?.ToString(); "
        "  var h := n?.GetHashCode(); "
        "  if s <> null then return 1; end; "
        "  if h <> null then return 2; end; "
        "  return 42; ");
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

// ═══════════ Accept side (§15): a real user class's `?.` form is unaffected ═
BEZEL_TEST(ffi_buffer_nco_4137, accept_side_user_class_nullable_receiver_unaffected) {
    const std::string p =
        "module M; "
        "class Widget public property Name: string := \"w\"; end Widget; "
        "class C public static method Main(): int "
        "  var w: Widget? := new Widget(); "
        "  var s := w?.ToString(); "
        "  if s <> \"M.Widget\" then return 1; end; "
        "  var wn: Widget? := null; "
        "  var sn := wn?.ToString(); "
        "  if sn <> null then return 2; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(ffi_buffer_nco_4137, accept_side_user_class_override_still_dispatches_nullable) {
    const std::string p =
        "module M; "
        "class Widget "
        "  public override method ToString(): string => \"custom\"; "
        "  public override method GetHashCode(): int => 7; "
        "end Widget; "
        "class C public static method Main(): int "
        "  var w: Widget? := new Widget(); "
        "  var s := w?.ToString(); "
        "  var h := w?.GetHashCode(); "
        "  if s <> \"custom\" then return 1; end; "
        "  if h = null or h! <> 7 then return 2; end; "
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(runAot(p).ran);
    BEZEL_EXPECT_EQ(run(p), 42);
}
