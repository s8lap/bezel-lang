// fix #7849 (§28.8.1 *The replacing `move`*, v0.336) — FLIPPED from a
// REJECTED row to an ACCEPTED row. The retired field-restoration window was
// the row's only ground.
//
// §28.8.1 (#1125): the qualified force-unwrap form `move this._log!` is the
// replacing `move` at a NULLABLE location, and it STORES `null`:
//   "Where the location's declared type is **nullable** ... the `with` clause
//    may be omitted: `move x.f` stores `null`." / "**No location ever holds a
//    hole** (K9.2) ... the *moved-from* state is a state of bindings only (K1).
//    Nothing therefore needs restoring after a `move` ..."
// The row's old diagnostic ("field `_log` left in moved-from state at method
// exit") belonged to that retired window.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile?; public constructor() _log := new Logfile(); end constructor; public method Take(): owned Logfile return move this._log!; end Take; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
