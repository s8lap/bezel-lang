// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, closure_capture_mutation_invalidates_across_call_spec_7_4).
//
// §7.4 (#1566): the probe. `s` is narrowed non-null, then nulled by a lambda
// invoked inside the block, then dereferenced. Before the fix this compiled
// clean and SIGSEGV'd at run time; it must now be rejected (re-narrow required).

module M; class C public static method Run(): int var s: string? := "hi"; var f := lambda() => do s := null; end; if s <> null then f(); return s.Length; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
// CHECK-SAME: (§7.4)
