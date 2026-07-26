// Companion ACCEPT control for #1827 (spec §32.5, v0.196).
//
// Mirrors `sema_extern_ffi_type__inline_array_of_primitive_ok_32_5.bzl`:
// the non-nullable `[N]int` shape must keep compiling after the #1827 fix
// layers `t.nullable` / `elem.nullable` rejections onto the InlineArray
// branch of `isFfiCompatibleType`. The fix is rejected-only — no narrowing
// of the accepted shape — so the [4]int parameter must remain green.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
extern "c"
  public method h(buf: [4]int): int;
  public method k(flag: bool, buf: [4]int): int;
end;

// NODIAG-NOT: error
// NODIAG-NOT: §
