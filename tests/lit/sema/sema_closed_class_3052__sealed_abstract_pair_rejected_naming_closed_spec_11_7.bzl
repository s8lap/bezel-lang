// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §11.7 (v0.242, #3052), verbatim: "(Through v0.241 the semantics were spelled
// as the modifier pair `sealed abstract`; that spelling is rejected, the
// diagnostic naming `closed`.)" Appendix C.1 states the same invariant: "The
// spelling `sealed abstract` is rejected, the diagnostic naming `closed`."
//
// The diagnostic is fix-it shaped — it prints the replacement declaration —
// because this is the MIGRATION case: an author who writes the pair means
// exactly `closed`, unlike the three `closed`+X combinations, which are
// ordinary contradictions.

module M; sealed abstract class Shape public abstract method Area(): int; end Shape;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Shape` may not be both `sealed` and `abstract`
// CHECK-SAME: write `closed class Shape`
// CHECK-SAME: (§11.7)
