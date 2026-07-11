// fix #1691 / §18.5 (v0.184): null interpolation holes must participate in
// alignment. The v0.184 delta to §18.5 says: "The alignment applies to the
// rendered text `null` exactly as to any other rendered text (it
// participates in field padding)." The spec's own normative example:
//
//   var x: double? := null;
//   Console.WriteLine($"[{x,8:F2}]");      // prints: [    null]
//   x := 3.0;
//   Console.WriteLine($"[{x,8:F2}]");      // prints: [    3.00]
//
// Before this fix, a null hole with an alignment/format spec rendered the
// bare "null" literal with NO padding applied (`[null]` instead of
// `[    null]`) — the nullBB arm of the nullable-VALUE-type branch in
// LlvmBackend_Exprs.cpp built the "null" string via lowerStringLiteral and
// branched straight to the merge block, skipping the width/alignment logic
// entirely.
//
// A second, more severe instance of the same defect existed for nullable
// REFERENCE-typed holes (`string?`, a nullable class/interface): WITH an
// alignment/format spec, the codegen fed the (possibly null) pointer
// straight into emitWithFormatSpec, which dereferences it unconditionally
// in the alignment path — a null value there SIGSEGV'd instead of
// rendering "[    null]". Both are fixed via a shared helper,
// Emitter::emitAlignOnly, which pads an already-rendered bz_string by the
// `,WIDTH` prefix of an encoded InterpPart::formatSpec without invoking the
// FORMAT portion (never dispatched on null, per §18.5 v0.184).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §18.5 v0.184 normative example, null case: `$"[{x,8:F2}]"` with
// `x: double? := null` must render "[    null]" — the alignment pads the
// rendered "null" text to the 8-character field. THIS IS THE BUG: without
// the fix, the alignment was dropped and the program printed "[null]",
// making `s = "[    null]"` false (return 2).
BEZEL_TEST(null_interp_alignment_1691, nullable_double_null_align_format) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: double? := null; "
        "  var s := $\"[{x,8:F2}]\"; "
        "  return (1 if s = \"[    null]\" else 2); "
        "end Main; end C;"), 1);
}

// Companion control from the same spec example: the PRESENT value still
// applies the format spec and pads normally ("[    3.00]"). Confirms the
// fix did not disturb the non-null (live) path.
BEZEL_TEST(null_interp_alignment_1691, nullable_double_present_align_format) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: double? := 3.0; "
        "  var s := $\"[{x,8:F2}]\"; "
        "  return (1 if s = \"[    3.00]\" else 2); "
        "end Main; end C;"), 1);
}

// Bare alignment (no `:FORMAT`) on a null nullable value: `$"[{x,8}]"` must
// also pad — the alignment portion of formatSpec (",8") is present whether
// or not a `:FORMAT` follows, and both routes through the same nullBB arm.
BEZEL_TEST(null_interp_alignment_1691, nullable_int_null_bare_align) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: int? := null; "
        "  var s := $\"[{x,6}]\"; "
        "  return (1 if s = \"[  null]\" else 2); "
        "end Main; end C;"), 1);
}

// Left alignment (negative width) on a null nullable value: `$"[{x,-8:F2}]"`
// must left-pad the FIELD, i.e. "null" followed by trailing spaces.
BEZEL_TEST(null_interp_alignment_1691, nullable_double_null_left_align) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: double? := null; "
        "  var s := $\"[{x,-8:F2}]\"; "
        "  return (1 if s = \"[null    ]\" else 2); "
        "end Main; end C;"), 1);
}

// Nullable REFERENCE type (string?), null, with a bare alignment spec:
// `$"[{s,8}]"` must render "[    null]", not crash. Before the fix this
// SIGSEGV'd (the null pointer was fed straight into emitWithFormatSpec's
// alignment path, which dereferences it unconditionally).
BEZEL_TEST(null_interp_alignment_1691, nullable_string_null_align) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string? := null; "
        "  var t := $\"[{s,8}]\"; "
        "  return (1 if t = \"[    null]\" else 2); "
        "end Main; end C;"), 1);
}

// Companion control: a PRESENT nullable-reference value still aligns
// correctly (unaffected by the null-branch fix).
BEZEL_TEST(null_interp_alignment_1691, nullable_string_present_align) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"hi\"; "
        "  var t := $\"[{s,8}]\"; "
        "  return (1 if t = \"[      hi]\" else 2); "
        "end Main; end C;"), 1);
}
