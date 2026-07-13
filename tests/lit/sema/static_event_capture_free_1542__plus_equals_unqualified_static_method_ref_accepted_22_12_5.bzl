// Ported from tests/sema/static_event_capture_free_1542_test.cpp
//   (BEZEL_TEST static_event_capture_free_1542, plus_equals_unqualified_static_method_ref_accepted_22_12_5).
//
// §22.12 rule 5: an unqualified static method reference (bare identifier
// resolving to a static method of the enclosing class, fix #1492's spelling)
// as the `+=` handler → accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Ev(N: int); class Bus public static event Signal: Ev; public static method OnSignal(e: Ev) end OnSignal; public static method Hook() Signal += OnSignal; end Hook; end Bus;

// NODIAG-NOT: error
// NODIAG-NOT: §
