// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, auto_property_read_keeps_capture_narrowing_spec_7_4).
//
// §7.4 (#1593): NO over-invalidation. A NON-virtual auto-property (`Val: int :=
// 0;`) getter is a compiler-generated backing-field load — it cannot transfer
// control to user code, so reading it does NOT drop a captured local's
// narrowing. This is the "exclude common auto reads" boundary the #1566 change
// reserved for #1593. Must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box public property Val: int := 42; end Box; class C public static method Run(box: Box): int var s: string? := "hi"; var f := () => do s := null; end; if s <> null then _ := box.Val; return s.Length; end; return -1; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
