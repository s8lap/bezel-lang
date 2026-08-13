// Codegen regression test for issue #2115 — §28.9 (string refcount discipline)
// + §35.4 (choices), v0.226.
//
// A VALUE-LIKE choice with a runtime-managed (`string`) payload leaked the +1
// payload string at scope end: the §35.3 construction lowering
// (LlvmBackend_Calls2.cpp) stores the producer's `bz_string*` raw into the
// case payload slot with no retain/release tracking, registerOwningChoiceDisposal
// skips value-like choices (its active-case switch is for `owned`-marked heap
// payloads only), and no scope-end walker visited the choice payload slot. So
// every construction whose payload was a PRODUCER string (`"a" + "b"`, an
// interpolation, a range slice) leaked once; a LITERAL payload did not (a
// literal is a sentinel-global whose release is a no-op) — which is how the
// equality lane's runAotChecked tests sidestepped this pre-existing gap.
//
// The fix arms a tag-switched bz_string_release walk over the ACTIVE case's
// string-typed payload fields — the value-like analog of
// emitOwningChoiceActiveCaseDisposal — registered for a value-like choice local
// iff (a) its initializer is a choice construction every string payload arg of
// which is a +1 producer or a literal (the slot owns +1 on the active case's
// strings) and (b) the value never escapes its scope (the shared
// StringEscapeWalker verdict — no copy / return / capture / case-match /
// reassignment, all of which would let a second release double-free).
//
// Spec authority: §28.9 (a runtime-managed payload follows the string refcount
// discipline), §35.4 v0.226 (choice classification / "disposal is defined by
// the active case"). All tests run via runAotChecked (ASan/LSan): the leak is a
// "detected memory leaks" headline → runAotChecked returns ran=false (the
// fails-without oracle); a clean run returns the program's exit code.

#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked / runAot — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::codegen::CodegenOptions;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic. clang is required to
// build this repo, so a skip is not expected — guard anyway (mirrors the
// choice_equality_control_flow_rework_2083 test).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// runAot with generated-code ASan on and LSan OFF — the SPEC_28_9_35_4 escape
// oracle. The gate's safety contract (a value-like choice whose value escapes
// via a plain method arg-pass is excluded from scope-end release) is asserted
// here under ASan (catches UAF/double-free) with leak detection deliberately
// disabled — the escape direction is the SAFE pre-fix behavior (a residual
// leak on the unreleased local's +1 payload string is the cost of never
// arming the release on a possibly-aliased slot). Mirrors the
// `runAotAsanNoLeak` helper in string_local_reassign_leak_1997_test.cpp.
JitResult runAotAsanNoLeak(std::string_view src) {
    static const bool toolchainOk =
        std::system("clang --version > /dev/null 2>&1") == 0;
    if (!toolchainOk) return runAotChecked(src);  // SKIP diagnostic
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    return runAot(src, opts);
}

}  // namespace

// ─── THE PIN: a producer (concat) string payload leaks at scope end pre-fix.
// `Msg.Note("he" + "llo")` mints a +1 heap string (the concat) stored raw into
// the Note case payload slot. Pre-fix the slot's bz_string* is never released
// (no scope-end walker visits choice payload slots) → LSan reports the leak →
// runAotChecked returns ran=false. Post-fix the active-case string release
// walk frees it once at scope end → clean. NB: a LITERAL payload cannot pin
// this (a literal is a sentinel-global whose release is a no-op) — the producer
// concat is load-bearing.
BEZEL_TEST(choice_value_like_string_payload_2115,
           spec_28_9_35_4_concat_payload_released_at_scope_end) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var a := Msg.Note(\"he\" + \"llo\"); "  // +1 concat stored as payload
        "  return 0; "
        "end Main; end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix: LSan leak → ran=false
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // clean run
}

// A producer payload in a NON-LEADING case (Empty is case 0; here a string-
// bearing case is declared second) — the active-case switch keys on the runtime
// TAG (not case declaration index alone), so the release arm fires for the
// constructed case regardless of where it sits in the declaration list. Same
// fails-without/passes-with contract as the leading-case pin.
BEZEL_TEST(choice_value_like_string_payload_2115,
           spec_35_4_active_case_switch_keys_on_runtime_tag) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Empty, Note(text: string); end Msg; "  // Note is case 1
        "class C public static method Main(): int "
        "  var a := Msg.Note(\"ab\" + \"cd\"); "
        "  return 0; "
        "end Main; end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A producer string payload alongside a payload-free case: a value-like choice
// whose other case carries no string. The Empty arm of the release switch
// collapses to the join (nothing to release); the Note arm releases its string.
// Constructing Empty (no payload) is clean by construction — no string was
// minted. Constructing Note here mints the +1 that must be reclaimed.
BEZEL_TEST(choice_value_like_string_payload_2115,
           spec_35_4_payload_free_active_case_releases_nothing) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var a := Msg.Note(\"x\" + \"y\"); "
        "  var b := Msg.Empty; "  // payload-free — no string minted
        "  return 0; "
        "end Main; end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// An interpolation payload (a +1 producer per §4.6, distinct from the concat
// form) stored into a case payload. Same scope-end release contract — the
// eligibility recognizes every §28.9 +1 string producer, not just `+`.
BEZEL_TEST(choice_value_like_string_payload_2115,
           spec_28_9_interpolation_payload_released_at_scope_end) {
    JitResult r = runAotChecked(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class C public static method Main(): int "
        "  var a := Msg.Note($\"{\"wo\"}{\"rld\"}\"); "  // +1 interpolation
        "  return 0; "
        "end Main; end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ─── ESCAPE GATE: a producer payload whose choice is passed (as a plain method
// arg) into a callee that field-stores it — the #1430 plain-arg relaxation
// must NOT apply for the value-like choice walk. A field store of a
// `string`-typed param retains into the field's bz_string* (the
// `lowerStringFieldStore` `emitStringRetain` discipline); a field store of a
// VALUE-LIKE-CHOICE-typed param is a raw aggregate memcpy with NO retain of the
// embedded payload — the callee field aliases the caller's SAME +1. So if the
// gate is removed and the relaxation runs unconditional in the choice walk,
// the caller's local is marked non-escaping and armed for scope-end release;
// at end of inner scope the +1 string is freed; the field's pointer dangles;
// the post-scope `h.TextLen()` reads freed memory → ASan UAF.
//
// Post-fix (gate on stringReleaseMode, mirroring #1994/#2023): the choice walk
// (stringReleaseMode=false) skips the relaxation and treats the arg as an
// ESCAPE — the local is excluded from scope-end release and its payload
// leaks (the safe pre-#2115 behavior, leak-over-UAF). The field's pointer
// stays valid → ASan-clean. Same gate the shared dict-materializer (#1028),
// grouping (#1930), and borrowing-Array (#1003) walks rely on for their
// conservative ESCAPE here. // per §28.9 (string refcount discipline), §35.4
// v0.226.
BEZEL_TEST(choice_value_like_string_payload_2115,
           spec_28_9_35_4_escape_gate_argpass_no_uaf) {
    JitResult r = runAotAsanNoLeak(
        "module M; "
        "choice Msg Note(text: string), Empty; end Msg; "
        "class Holder public s: Msg; "
        "  public method Set(c: Msg) this.s := c; end Set; "
        "  public method TextLen(): int "
        "    return (case this.s of Note(var t) => t.Length, _ => 0); "
        "  end TextLen; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  do "
        "    var c := Msg.Note(\"xyzw\"); "  // +1 concat stored as payload
        "    h.Set(c); "                    // plain method arg — escape gate
        "  end; "
        "  var len := h.TextLen(); "        // reads field text AFTER c's release
        "  if len = 4 then return 0; end; "
        "  return 1; "
        "end Main; end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix (no gate): ASan UAF on h.TextLen() → ran=false
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // post-fix (gate): c excluded, no UAF, len = 4
}

// ===========================================================================
// #3150 / #3144 review nit N1 — THE GENERIC ARM, which no test had.
//
// `LlvmBackend_Setup.cpp`'s `valueLikeChoiceLocalMintsStringPayload` decided
// #2115's substitution with
//
//     dynamic_cast<const ast::TypeDecl*>(rt->declaration)
//
// casting a `sema::TypeSymbol*` to an `ast::TypeDecl*`. Those are unrelated
// hierarchies and a TypeSymbol is only ever built by `makeSymbol<TypeSymbol>`,
// so no complete object can derive from both: the cast was **always null, on
// every path, local and imported alike**. #2115's generic-payload substitution
// had therefore NEVER ONCE EXECUTED — the guarded block was dead from the day
// it was written, and the file's five existing arms are all non-generic, so
// nothing noticed.
//
// #3144 replaced the dead cast with the symbol-side case list. That made the
// path live for the first time, which is a BEHAVIOUR change and needs a
// behavioural pin — the ratchet's "class-2 count is now 0" pins the code SHAPE
// only. This arm is that pin, and it is a purely LOCAL program: the defect was
// never import-specific, which is why it survived a cross-module audit.
//
// Measured on this base: pre-fix `AddressSanitizer: 3590 byte(s) leaked in 200
// allocation(s)`, rc=1; post-fix clean, rc=0. // per §35.4 / §28.9, v0.254
// ===========================================================================
BEZEL_TEST(choice_value_like_string_payload_2115,
           spec_35_4_generic_value_like_payload_released_3150) {
    JitResult r = runAotAsanNoLeak(
        "module M; "
        "choice Opt<T> Some(v: T), None; end Opt; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var n := 0; "
        "  while i < 100 do "
        "    i := i + 1; "
        "    var o := Opt<string>.Some($\"v{i}\"); "   // +1 string as a GENERIC payload
        "    n := n + (case o of Some(var s) => s.Length, _ => 0); "
        "  end while; "
        "  if n > 0 then return 0; end; "
        "  return 1; "
        "end Main; end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix: LSan reports the leak → ran=false
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
