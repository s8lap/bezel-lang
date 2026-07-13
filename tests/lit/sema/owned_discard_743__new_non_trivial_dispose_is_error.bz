// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, new_non_trivial_dispose_is_error).
//
// §21.9: `new T();` where T is non-trivial-dispose (a destructor) — hard error.

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class C public static method Main(): int new R(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this expression produces an owned value that requires disposal (a non-trivial-dispose class or a move-only owning array); discarding it as a statement leaks the resource — bind it (`using var _ := expression;`), call `.Dispose()`, or transfer it (`move`/`return`)
// CHECK-SAME: (§21.9)
