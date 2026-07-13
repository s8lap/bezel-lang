// Ported from tests/sema/static_event_capture_free_1542_test.cpp
//   (BEZEL_TEST static_event_capture_free_1542, instance_event_plus_equals_capturing_field_still_accepted_22_12_5).
//
// §22.12 rule 5's last sentence: `instanceEvent += handler` with a capturing
// handler (inside the declaring class, on the EventSource<T> face) must also
// remain accepted — the rule targets `static event` only.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Ev(N: int); class Src public Total: int := 0; public event Signal: Ev; public method Hook() Signal += (e) => do Total := Total + e.N; end; end Hook; end Src;

// NODIAG-NOT: error
// NODIAG-NOT: §
