// Regression test for #2222 — §32.6 (string marshalling), v0.226. A non-ASCII
// Bezel `string` (storage width > 1, e.g. "AĀB") passed to a `CString` extern
// parameter (§32.5) is re-encoded to a fresh malloc'd UTF-8 buffer via
// `bz_string_to_utf8z` so the C side reads a valid null-terminated byte
// sequence (the raw width>1 data pointer has interior NUL bytes from
// 2-byte code-point storage and would truncate the C-side read). The buffer
// is the caller's responsibility per the §32.6 / stdlib §10.1 caller-owns
// contract (a CString extern parameter is borrowed by the C callee), so the
// buffer must be freed AFTER the call returns. Before the fix the buffer
// was never freed — every non-ASCII CString parameter in a loop leaked a
// malloc'd UTF-8 buffer (the #2222 LSan report: per-iteration leak; 100
// iterations × 13 bytes = 1300 bytes). The fix adds a per-parameter
// `entryAlloca` slot, stores the marshalled buffer in the slot in the
// width>1 basic block, and emits a null-guarded `free()` after the call.
// Width-1 (ASCII) strings don't allocate (they hand over the Bezel
// refcounted raw data pointer), so the slot stays null and the post-call
// free is a no-op. Symmetric with the `write(2)` CString consumer in
// `LlvmBackend_Calls.cpp:53` which `free()`s the same `bz_string_to_utf8z`
// buffer at its use site. AOT+ASan/LSan oracle over 100 FFI calls.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// FAIL-WITHOUT: LSan flags 100 × 13-byte leaks from bz_string_to_utf8z in
// main, so r.ran flips to false. PASS-WITH: each call's marshalled buffer
// is freed post-call, no leaks, r.ran == true. Returns total (4 × 100 = 400
// → 400 mod 256 = 144) on the success path.
int runProgram(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §32.6 / #2222: a non-ASCII Bezel string passed to a CString extern
// parameter does not leak the bz_string_to_utf8z buffer. 100 iterations of
// strlen("AĀB") — without the fix LSan reports 100 leaked allocations
// (~13 bytes each, totalling ~1300 bytes). With the fix each call's
// marshalled UTF-8 buffer is released after strlen returns.
BEZEL_TEST(cstring_param_buf_free_2222, nonascii_cstring_param_in_loop_freed_32_6) {
    const std::string src =
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var s := \"A\\u0100B\"; "
        "  var total := 0; var i := 0; "
        "  while i < 100 do "
        "    total := total + strlen(s); "
        "    i := i + 1; "
        "  end; "
        "  return total; "
        "end Main; end C;";
    // 4 bytes × 100 = 400; 400 mod 256 = 144. ran==false → -999 means
    // ASan/LSan detected a leak and the binary exited non-clean.
    BEZEL_EXPECT_EQ(runProgram(src), 144);
}

// §32.6 / #2222 control: a width-1 (ASCII) CString parameter never
// allocates (the raw Bezel data pointer is null-terminated UTF-8 and is
// handed over directly) — the post-call free is a no-op, no leak with or
// without the fix. Pins that the slot-allocation/free path doesn't disturb
// the existing width-1 fast path.
BEZEL_TEST(cstring_param_buf_free_2222, ascii_cstring_param_in_loop_clean_32_6) {
    const std::string src =
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 100 do "
        "    total := total + strlen(\"abc\"); "
        "    i := i + 1; "
        "  end; "
        "  if total = 300 then return 1; end; "
        "  return 2; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runProgram(src), 1);
}

// §32.6 / #2222: a single (non-loop) non-ASCII CString parameter also
// doesn't leak. With the fix the marshalled buffer is freed after the
// single call; without the fix a single buffer is leaked. Pin: exit code
// 4 (strlen("AĀB") = 4).
BEZEL_TEST(cstring_param_buf_free_2222, single_nonascii_cstring_param_freed_32_6) {
    const std::string src =
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var s := \"A\\u0100B\"; "
        "  return strlen(s); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runProgram(src), 4);
}

// #2222 residue (FILED: 2026-07-28) / §28.9 / §32.6 / fix #2222 scope-end release gap:
// the original #2222 LSan report was 32+104 bytes from the FFI + string-concat
// repro — a bz.string header (32 bytes direct) + its 103-byte data buffer (104
// bytes indirect, including the null terminator). The #2222 fix closed the
// bz_string_to_utf8z buffer leak (the non-ASCII marshalled buffer) but left
// behind a different leak in the ASCII + concat case: the StringEscapeWalker
// conservatively marked the `var s := "abc"; s := s + "x" 100x; strlen(s);`
// local as ESCAPED at the FFI call (the pre-fix walker key only on a
// compiled MethodSymbol and treats every other callee as escaped), so the
// scope-end release of `s` never armed and the FINAL concat result leaked
// at function return. Both controls are non-escaping paths that the
// post-StringEscapeWalker-fix relaxation admits — the C side does not
// retain, and the ASCII marshal hands over the raw bz.string data pointer
// as a borrow. Pre-fix: 32+104 byte LSan leak (the final concat result).
// Post-fix: ran==true, exit code 103. // per §28.9, §32.5, §32.6, #2222
BEZEL_TEST(cstring_param_buf_free_2222, ascii_concat_then_ffi_scope_end_release) {
    const std::string src =
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var s := \"abc\"; "
        "  var i := 0; "
        "  while i < 100 do "
        "    s := s + \"x\"; "
        "    i := i + 1; "
        "  end; "
        "  return strlen(s); "
        "end Main; end C;";
    // s.Length == 103 (post-loop), strlen("abc" + 100*"x") == 103. ran==false
    // → -999 means ASan/LSan detected a leak and the binary exited non-clean.
    BEZEL_EXPECT_EQ(runProgram(src), 103);
}

// #2222 residue / plain read of a string local at an FFI call from a
// non-function-body scope (a `do` block) — same walker fix admits the
// `do strlen(s); end` shape without leaking the final concat. Also runs
// the FFI at non-ASCII widths with no concat, the analogous ASCII+FFI
// without concat (already clean as a control), and a chained-FFI case
// (multiple borrows in sequence stay non-escaping). Each pre-fix would
// either fail (LSan) or run into the conservative-ESCAPE posture; each
// post-fix returns normally with ran==true. // per §28.9, §32.5, §32.6, #2222
BEZEL_TEST(cstring_param_buf_free_2222, ffi_borrow_in_do_block_no_concat_clean) {
    const std::string src =
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var s := \"abc\"; "
        "  do var n := strlen(s); end; "
        "  return s.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(runProgram(src), 3);
}
