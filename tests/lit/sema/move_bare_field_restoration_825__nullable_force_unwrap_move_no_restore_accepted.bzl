// fix #7849 (§28.8.1 *The replacing `move`*, v0.336) — FLIPPED from a
// REJECTED row to an ACCEPTED row. The retired field-restoration window was
// the row's only ground.
//
// §28.8.1 (#1125): `move _log!` — a force-unwrap (null-forgiving `!`) of a
// NULLABLE owned field — is a move of `_log` exactly like `move _log`, and at
// a nullable location the take STORES `null`:
//   "Where the location's declared type is **nullable** ... the `with` clause
//    may be omitted: `move x.f` stores `null`." / "**No location ever holds a
//    hole** (K9.2) ... the *moved-from* state is a state of bindings only (K1).
//    Nothing therefore needs restoring after a `move` ..."
// The row's old diagnostic ("field `_log` left in moved-from state at method
// exit") belonged to that retired window. The `!` is the null test the
// replacing `move` runs immediately before the exchange (§17.8) — it does not
// make the field hold a hole.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile?; public constructor() _log := new Logfile(); end constructor; public method Take(): owned Logfile return move _log!; end Take; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
