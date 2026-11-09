// fix #7849 (§28.8.1 *The replacing `move`*, v0.336) — FLIPPED from a
// REJECTED row to an ACCEPTED row. The retired field-restoration window was
// the row's only ground.
//
// §28.8.1 / #867: a NULLABLE owned field's bare take STORES `null`, so the
// field's post-take state is `null` on EVERY path — the branch that restores
// `null` explicitly and the branch that does not agree:
//   "Where the location's declared type is **nullable** ... the `with` clause
//    may be omitted: `move x.f` stores `null`." / "A nullable location a bare
//    take leaves holding `null` is an ordinary null field: its holder's
//    cascade disposes nothing through it, and a later read reads `null`."
// There is no restoration window left to violate on the `c == false` path, so
// the path-sensitivity that #867 added has no ground under it either. The
// row's old diagnostic ("field `_log` left in moved-from state at method
// exit") belonged to that retired window.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile?; public constructor() _log := new Logfile(); end constructor; public method Maybe(c: bool): owned Logfile? var d := move _log; if c then _log := null; end; return d; end Maybe; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
