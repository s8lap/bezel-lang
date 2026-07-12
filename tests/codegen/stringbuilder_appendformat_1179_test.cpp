// stdlib §3.1 StringBuilder.AppendFormat — fix #1179.
//
// AppendFormat(format, args) appends the formatted result: .NET-style composite
// placeholders {N} are substituted with args[N]'s string form, and the resulting
// text is appended to the builder's parts list (like Append). A no-placeholder
// format contributes its literal text. Before the fix the call was a SILENT
// NO-OP (appended nothing): it had no codegen/runtime dispatch case and sema did
// not register its StringBuilder return type, so even chained calls dropped the
// whole subtree. These tests pin both the statement form (the #1179 repro) and
// the chaining form (AppendFormat returns `this`, §3.1).
//
// StringBuilder is an owned type, so test programs bind it with `using var`
// (§28.3); the Array<Object> args collection is likewise `using var`.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// -------------------------------------------------------------------------
// §3.1 / #1179: the reported repro — a no-placeholder format must contribute
// its literal text. Without the fix the AppendFormat appends nothing and the
// builder reads "XY" (Length 2); with it "XabY" (Length 4).
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_appendformat_spec_3_1, no_placeholder_contributes_text) {
    // Returns the builder's Length (#1179: was 2, now 4).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"X\"); "
        "  using var args := new Array<Object>(); "
        "  args.Add(7); "
        "  sb.AppendFormat(\"ab\", args); "
        "  sb.Append(\"Y\"); "
        "  return sb.ToString().Length; "
        "end Main; end C;"), 4);
}

// -------------------------------------------------------------------------
// §3.1: {N} substituted with args[N].ToString().
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_appendformat_spec_3_1, single_placeholder) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  using var args := new Array<Object>(); "
        "  args.Add(42); "
        "  sb.AppendFormat(\"v={0}\", args); "
        "  return (1 if sb.ToString() = \"v=42\" else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_sb_appendformat_spec_3_1, multiple_placeholders) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  using var args := new Array<Object>(); "
        "  args.Add(1); "
        "  args.Add(2); "
        "  sb.AppendFormat(\"{0}+{1}=3\", args); "
        "  return (1 if sb.ToString() = \"1+2=3\" else 0); "
        "end Main; end C;"), 1);
}

// -------------------------------------------------------------------------
// §3.1: methods return `this` to allow chaining. AppendFormat between Appends
// must contribute to the same builder (was dropped entirely before the fix
// because sema did not type AppendFormat's result as StringBuilder).
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_appendformat_spec_3_1, chaining_returns_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  using var args := new Array<Object>(); "
        "  args.Add(9); "
        "  var r := sb.Append(\"a\").AppendFormat(\"{0}\", args).Append(\"b\"); "
        "  return (1 if r.ToString() = \"a9b\" else 0); "
        "end Main; end C;"), 1);
}

// -------------------------------------------------------------------------
// §3.1: escaped braces {{ }} render as literal braces; literal text around
// placeholders is preserved verbatim.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_appendformat_spec_3_1, escaped_braces_and_literal_text) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  using var args := new Array<Object>(); "
        "  args.Add(9); "
        "  sb.AppendFormat(\"{{x}}={0}\", args); "
        "  return (1 if sb.ToString() = \"{x}=9\" else 0); "
        "end Main; end C;"), 1);
}

}  // namespace
