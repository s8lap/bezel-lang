// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, widened_override_staying_widened_down_chain_ok).
//
// §12.1 (v0.181, #1560): the positive half of the monotone chain — once
// Mid widens to `string?`, Grand overriding with `string?` again (staying
// widened, not narrowing) is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public virtual method F(a: string) _ := 1; end F; end Base; class Mid : Base public override method F(a: string?) _ := 2; end F; end Mid; class Grand : Mid public override method F(a: string?) _ := 3; end F; end Grand;

// NODIAG-NOT: error
// NODIAG-NOT: §
