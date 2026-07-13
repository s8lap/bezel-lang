// Ported from tests/sema/toplevel_visibility_test.cpp
//   (BEZEL_TEST sema_toplevel_vis, nested_private_type_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public class Outer private class Inner end Inner; end Outer;

// NODIAG-NOT: error
// NODIAG-NOT: §
