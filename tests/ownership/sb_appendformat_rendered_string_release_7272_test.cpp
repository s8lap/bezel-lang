// Spec-derived regression test for #7272 — `StringBuilder.AppendFormat`
// dispatches the Object vtable's renderer (vtable slot 3) on every argument
// and copies the rendered bytes into its own buffer WITHOUT discharging the
// reference the renderer handed it. The rendered string is therefore leaked
// on every call.
//
// THE CONTRACT THE FIX LEANS ON. The uniform per-type thunk that answers
// vtable slot 3 is documented in `src/codegen/LlvmBackend_String.cpp`'s
// `lowerObjectToString`: "this function's contract — relied on by every one
// of its callers — is that the value it hands back is a fresh `+1` the
// consumer discharges once … The other two arms honour that (the vtable
// thunks mint, a literal is an immortal sentinel on which retain/release are
// no-ops)". The runtime helper `bz_sb_append_format` is such a consumer: it
// borrows the rendered bytes and copies them into its own buffer, so the
// reference is ITS to discharge. The renderers' own minting behaviour is the
// other half of the anchor: `bz_strbuilder_to_string_obj` and the primitive
// renderers mint a fresh +1, and the §8.5 class default mints a literal.
//
// NOTE ON THE SPEC ANCHOR. §28.3 K1.6 (v0.344) says the runtime-managed
// types — `string` included — "carry no classification and no obligation", and
// §28.9 (v0.344) says of them that "how the runtime manages their lifecycle
// (reference counting, region allocation, or other) is not part of this
// specification". So the SPEC's ownership kernel imposes NO disposal
// obligation on the rendered string, and K1.1's "owned — the holder carries
// the disposal obligation" does NOT apply to it (a `string` is not a
// classified value). The release this test pins rests on the slot-3 contract
// above and on the renderers' own minting behaviour.
//
// THE DEFECT. `bz_sb_append_format` (src/runtime/bezel_runtime_strings.cpp)
// resolved each argument's string form ONCE into a `{data, len}` cache and
// then memcpy'd out of the rendered string's OWN data buffer — and never
// released it. The rendered value is a fresh +1 heap string for every
// renderer that mints one (`bz_strbuilder_to_string_obj` for a StringBuilder
// argument, the primitive renderers for an int/char argument) or an immortal
// sentinel for the ones that return a literal (the §8.5 default type name).
// `bz_string_release` is null-safe AND sentinel-safe, so a uniform release
// over every rendered argument discharges exactly the minted references and
// leaves the sentinels alone.
//
// MEASURED, on the base arm (the AOT ASan/LSan leg), for THIS file's own
// program — which appends the width-1 "x", so the rendered data buffer is
// 2 bytes:
//   one `sb.AppendFormat("{0}", args)` with a StringBuilder argument
//     => `SUMMARY: AddressSanitizer: 34 byte(s) leaked in 2 allocation(s).`
//        (the 32-byte string header `bz_sb_string_from_cps` +
//         the 2-byte width-1 data buffer)
//   100 such calls in a loop
//     => `SUMMARY: AddressSanitizer: 3400 byte(s) leaked in 200 allocation(s).`
// With the fix both are clean and the printed output is byte-for-byte
// unchanged (the over-rejection control below pins that).
// (The ISSUE's own probe appends the WIDE "日", whose width-2 data buffer is
// 4 bytes, so the same site measures 36 B / 2 allocations there — the same
// two allocations, the same missing release, only the buffer's width
// differs.)
//
// SCOPE. This is the `AppendFormat` HALF of the widened class. The other two
// shapes the issue names — `using var t := o.ToString()` and
// `using var t := sb.ToString()` — leak the SAME amount and are a DIFFERENT
// path: the missing release there is the scope-end walk in
// `src/codegen/LlvmBackend_Setup.cpp`'s `Emitter::collectStringReleaseEligible`
// (its producer-arm gate `if (v.init && !v.isUsing)` excludes every
// `using var` string local), which is OUTSIDE this lane's territory. They are
// NOT pinned here, because a row that fails on both arms is not a regression
// test; they are reported to the coordinator for sequencing.
//
// DELIBERATELY NOT FIXED HERE (both named by the issue and out of scope):
//   * #7244 — `AppendFormat` SEGFAULTS when the argument is a `string` (a
//     string has no vtable, so the slot-3 call jumps into the string's data).
//     That is a different failure mode and a compiler crash.
//   * #7229's residual (b) — `AppendFormat` renders a wide argument
//     BYTE-WISE. The byte count the helper copies is unchanged by this fix.
//
// THE SPELLING MATRIX (measured on both arms, AOT ASan/LSan leg). The fix
// discharges the RENDER's reference for every argument kind whose renderer
// mints one; the rows it does NOT change are named here so they are not
// lost:
//   StringBuilder argument  base 36 B / 2 allocs   -> fix CLEAN   (pinned)
//   class with a user ToString override
//                          base 35 B / 2 allocs   -> fix CLEAN   (pinned)
//   class with the §8.5 default (a literal)
//                          base CLEAN             -> fix CLEAN   (pinned)
//   `int` / `char` / `decimal` / `enum` / `record` argument
//                          the RENDER's bytes leak on the base arm and are
//                          CLEAN after the fix; a SEPARATE, pre-existing
//                          leak remains — the BOX the value is stored in
//                          (24 B for int/char/enum, 32 B for decimal/record).
//                          That leak is present with NO AppendFormat call at
//                          all (a program that only does `args.Add(7)` leaks
//                          the same 24 B), so it is a DIFFERENT defect and is
//                          NOT pinned by a row here.
//   `string` argument      #7244: SEGFAULT (see above) — unchanged by this
//                          fix, verified BYTE-IDENTICAL on both arms (the
//                          fault addresses are the same to the byte).
//
// per §28.9, v0.344, and the slot-3 contract documented in
// src/codegen/LlvmBackend_String.cpp's `lowerObjectToString`

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The issue's own repro, loop-amplified: 100 `AppendFormat` calls whose
// argument is a StringBuilder, so the leak is 100 renders rather than one.
// WITHOUT the fix `runAotChecked` reports `r.ran == false` with the LSan
// summary `SUMMARY: AddressSanitizer: 3400 byte(s) leaked in 200
// allocation(s).`; WITH the fix it reports the printed total and
// `r.ran == true`.
BEZEL_TEST(sb_appendformat_rendered_string_release_7272,
            appendformat_stringbuilder_arg_loop_clean) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"x\"); "
        "    using var args: Array<Object> := new Array<Object>(); "
        "    args.Add(sb); "
        "    using var sb2 := new StringBuilder(); "
        "    for var i in 1..100 do "
        "      sb2.AppendFormat(\"{0}\", args); "
        "    end; "
        "    var t := sb2.ToString(); "
        "    Console.WriteLine(t.Length.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The over-rejection control: the FIX MUST NOT CHANGE ANY PROGRAM'S
    // OUTPUT — only the leak disappears. 100 renders of the one-code-point
    // "x" ⇒ the builder holds 100 characters.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// The single-shot shape, pinned exactly as the issue filed it: one
// `AppendFormat` call whose argument is a StringBuilder. On the base arm
// this leaks 36 B / 2 allocations
// (`SUMMARY: AddressSanitizer: 34 byte(s) leaked in 2 allocation(s).`) and
// `r.ran` is false; with the fix it is clean. The printed length is the
// builder's own content, identical on both arms.
BEZEL_TEST(sb_appendformat_rendered_string_release_7272,
            appendformat_stringbuilder_arg_single_shot_clean) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"x\"); "
        "    using var args: Array<Object> := new Array<Object>(); "
        "    args.Add(sb); "
        "    using var sb2 := new StringBuilder(); "
        "    sb2.AppendFormat(\"{0}\", args); "
        "    var t := sb2.ToString(); "
        "    Console.WriteLine(t.Length.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// CONTROL — the program does not call the renderer at all. It must be clean
// on BOTH arms: it pins that the row above fails on the base arm because of
// the RENDER (the slot-3 dispatch and its missing release), not because of
// the builder, the array, or the harness.
BEZEL_TEST(sb_appendformat_rendered_string_release_7272,
            control_no_renderer_call_clean) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"x\"); "
        "    Console.WriteLine(sb.Length.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// CONTROL — the SAME render, reached WITHOUT `AppendFormat`: a `var`
// binding of `sb.ToString()` (the builder's own ToString, not the Object
// vtable's). This is the house rule measured before asserting anything about
// it: a `var`-bound string local is discharged at scope end by the codegen's
// own release walk, so this shape is CLEAN on both arms. It is the control
// that shows the fix did not change the direct route's behaviour — and the
// shape the `using var` twin of the issue (a DIFFERENT path, out of this
// lane's territory) is measured against.
BEZEL_TEST(sb_appendformat_rendered_string_release_7272,
            control_var_binding_of_direct_render_clean) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"x\"); "
        "    var t := sb.ToString(); "
        "    Console.WriteLine(t.Length.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// CONTROL — the Object-vtable route, bound with `var` (not `using var`).
// The Object-vtable route and the direct route answer the same rendering
// contract (§8.5: "The default and `FullName` are one rendering contract, not
// two"), so this renders the same text as the row above; the `var` binding's
// scope-end release discharges it, so the shape is CLEAN on both arms. This is the control for the fix's release
// count: if the helper released one reference too many, this row (and the
// `AppendFormat` rows) would report a heap-use-after-free instead of
// passing.
BEZEL_TEST(sb_appendformat_rendered_string_release_7272,
            control_var_binding_of_object_route_clean) {
    std::string src =
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"x\"); "
        "    var o: Object := sb; "
        "    var t := o.ToString(); "
        "    Console.WriteLine(t.Length.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// The USER-OVERRIDE arm of §8.5: a class with its own `ToString(): string`
// returns a value the method's return-statement minted EXACTLY +1 for (the
// #429 return-escape rule), so the helper's uniform release must discharge
// THAT reference too. Without the fix the rendered string leaks here exactly
// as the StringBuilder arm does; with it the shape is clean.
//
// MEASURED on the base arm: `SUMMARY: AddressSanitizer: 35 byte(s) leaked in
// 2 allocation(s).` (the +1 the user's `ToString()` minted — a fresh +1
// string header and its 3-byte "PQ" data buffer). With the fix: clean, and
// the printed length ("PQ") is unchanged.
BEZEL_TEST(sb_appendformat_rendered_string_release_7272,
            appendformat_user_override_render_clean) {
    std::string src =
        "module M; import System; "
        "class P "
        "  public override method ToString(): string "
        "    return \"P\" + \"Q\"; "
        "  end ToString; "
        "end P; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new P(); "
        "    using var args: Array<Object> := new Array<Object>(); "
        "    args.Add(p); "
        "    using var sb2 := new StringBuilder(); "
        "    sb2.AppendFormat(\"{0}\", args); "
        "    var t := sb2.ToString(); "
        "    Console.WriteLine(t.Length.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// CONTROL — the §8.5 DEFAULT arm: a class with NO `ToString` override
// renders its module-path-qualified type name, which codegen emits as a
// STRING LITERAL — an IMMORTAL SENTINEL, not a heap string (the same
// sentinel the `lowerStringLiteral` helpers mint). The helper's uniform
// `bz_string_release` must therefore be a NO-OP on it: releasing a sentinel
// would be an over-release.
//
// This is the row that pins the release's SAFETY, not just its presence:
// `bz_string_release` returns early on the sentinel, so the shape is clean
// and the rendered text is unchanged. If the fix released a value the
// renderer did not mint, this row would report a heap-use-after-free.
//
// The row ASSERTS THE RENDERED TEXT, not merely that the program ran: the
// §8.5 default for a class is its module-path-qualified type name, so the
// builder holds exactly "[M.Q]" — the qualified name of `Q` in module `M`,
// wrapped in the format's literal brackets. Asserting the text (not only
// r.ran and the exit code) is what makes this row an over-rejection
// control: a release that corrupted the sentinel's bytes would change the
// printed text.
BEZEL_TEST(sb_appendformat_rendered_string_release_7272,
            control_default_class_render_sentinel_clean) {
    std::string src =
        "module M; import System; "
        "class Q "
        "end Q; "
        "class C "
        "  public static method Main(): int "
        "    using var q := new Q(); "
        "    using var args: Array<Object> := new Array<Object>(); "
        "    args.Add(q); "
        "    using var sb2 := new StringBuilder(); "
        "    sb2.AppendFormat(\"[{0}]\", args); "
        "    var t := sb2.ToString(); "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[M.Q]\n"));
}

} // namespace
