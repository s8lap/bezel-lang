// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/move_bare_field_restoration_825_test.cpp
//   (BEZEL_TEST move_bare_field_restoration_825, nullable_force_unwrap_move_no_restore_rejected).
//
// §28.8.1 (#1125): `move _log!` — a force-unwrap (null-forgiving `!`) of a
// nullable owned field — is a move of `_log` exactly like `move _log`, so the
// field is left moved-from and must be restored before exit. Before the fix the
// `!` form (a PostfixBangExpr wrapping the field) bypassed the field-move
// recognition entirely, so this compiled with NO diagnostic — and the owner's
// auto-disposer then double-freed the moved-out object (deterministic SIGSEGV).
// It must be rejected with the same §28.8.1 diagnostic as the bare form.

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile?; public constructor() _log := new Logfile(); end constructor; public method Take(): owned Logfile return move _log!; end Take; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `_log` left in moved-from state at method exit
// CHECK-SAME: (§28.8.1 — reassign the field before exiting)
