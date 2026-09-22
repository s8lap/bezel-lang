// fix #4607 — spec §32.5 *Boundary `out` and `ref`* + §22.4 (v0.306).
//
// THE RULE. §32.5 admits the §22.4 inline declaration at a boundary `out`
// parameter: "The inline declaration `out var x` (§22.4) and the discard
// `out _` (§22.6) are admitted."
//
// THE DEFECT. Sema resolved a call's formal parameter types from two sources —
// a local callee's `ast::MethodDecl`, and an imported callee's
// `ParameterSymbol::resolvedDeclType` (#2657). An `extern` callee is an
// `ExternDeclSymbol` whose formals live on `ast::ExternDecl::params`, which is
// neither, so the inline recipient's `LocalSymbol::declType` stayed null. Both
// of #2657's consequences followed: codegen's `ensureOutVarSlots` fell through
// to its `i32()` DEFAULT — a 4-byte stack slot the NATIVE callee then writes
// 8 bytes through — and the recipient carried no `exprType`.
//
// The `int` instantiation is deliberately NOT tested: `i32()` is accidentally
// the correct slot for `out exp: int`, so such a row would pass on both sides
// of the fix. (The by-address rule it would otherwise pin is covered by
// ffi_boundary_out_ref_by_address_1706_test.cpp.)
//
// MEASURED PRE-FIX BEHAVIOUR, which is why the two rows use different oracles:
//   * `sincos(0.0, out var s, out var c)` — two 8-byte writes into two 4-byte
//     slots — printed `s=0 c=0` on the PLAIN build (c must be 1). A value
//     oracle is enough, so that row is `runAot`.
//   * `strtol(s, out var e, 10)` printed the CORRECT `v=42 endSet=1` on the
//     plain build: the 4-byte overflow lands in frame padding and is tolerated.
//     Only ASan sees it, so that row is `runAotChecked`:
//         ERROR: AddressSanitizer: stack-buffer-overflow ... WRITE of size 8
//           #0 StrtolFixAndCheck(void*, char const*, char**, char*, int)
//           [320, 324) '' <== Memory access at offset 324 overflows this ...

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

// §32.5 / §22.4: `out var e` at an `out endptr: IntPtr` parameter. IntPtr is
// pointer-sized — 8 bytes on the host target (§32.7) — and the pre-fix slot
// was 4. The ASan-linked oracle is the one that sees it; a plain run of this
// same program is green on both sides of the fix.
BEZEL_TEST(codegen_ffi_out_var_slot,
           out_var_intptr_slot_is_pointer_sized_spec_32_5_4607) {
    auto r = runAotChecked(
        "module M; "
        "extern \"libc\" method strtol(nptr: CString, out endptr: IntPtr, "
        "                              radix: int): long; end; "
        "class C public static method Main(): int "
        "  using var s := CString.From(\"42rest\"); "
        "  var v := strtol(s, out var e, 10); "
        "  var endSet := 0; "
        "  if e <> IntPtr.Zero then endSet := 1; end; "
        "  Console.WriteLine($\"v={v} endSet={endSet}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("v=42 endSet=1\n"));
}

// §32.5 / §22.4: two `out var` recipients of `double` in one call. glibc's
// `sincos(0.0, &s, &c)` yields (0.0, 1.0); pre-fix each recipient had a 4-byte
// slot and the program printed "s=0 c=0".
BEZEL_TEST(codegen_ffi_out_var_slot,
           out_var_double_slots_are_eight_bytes_spec_32_5_4607) {
    auto r = runAot(
        "module M; "
        "extern \"libc\" method sincos(x: double, out sinv: double, "
        "                              out cosv: double): (); end; "
        "class C public static method Main(): int "
        "  sincos(0.0, out var s, out var c); "
        "  var ok := 0; "
        "  if s = 0.0 then ok := ok + 1; end; "
        "  if c = 1.0 then ok := ok + 2; end; "
        "  Console.WriteLine($\"s={s} c={c} ok={ok}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("s=0 c=1 ok=3\n"));
}
