// Ported from tests/sema/move_bare_field_restoration_825_test.cpp
//   (BEZEL_TEST move_bare_field_restoration_825, nullable_force_unwrap_move_null_restore_accepted).
//
// §28.8.1 (#1125 / spec line 3529): the canonical nullable-owned-field detach
// `move _log!` immediately followed by `_log := null` IS the spec-endorsed
// pattern and must compile cleanly (the field is restored before exit).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile?; public constructor() _log := new Logfile(); end constructor; public method Take(): owned Logfile var d := move _log!; _log := null; return d; end Take; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
