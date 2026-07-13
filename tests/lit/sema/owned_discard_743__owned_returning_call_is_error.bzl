// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/owned_discard_stmt_743_test.cpp
//   (BEZEL_TEST owned_discard_743, owned_returning_call_is_error).
//
// §21.9: a call returning `owned T` (T non-trivial-dispose) discarded — error.

module M; class R public _h: int := 0; destructor _h := 1; end destructor; end R; class F public static method Make(): owned R return new R(); end Make; end F; class C public static method Main(): int F.Make(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this expression produces an owned value that requires disposal (a non-trivial-dispose class or a move-only owning array); discarding it as a statement leaks the resource — bind it (`using var _ := expression;`), call `.Dispose()`, or transfer it (`move`/`return`)
// CHECK-SAME: (§21.9)
