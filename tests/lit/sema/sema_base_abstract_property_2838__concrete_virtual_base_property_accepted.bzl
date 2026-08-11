// Ported from tests/codegen/base_abstract_property_2838_test.cpp
//   (BEZEL_TEST codegen_base_abstract_property_2838,
//    concrete_virtual_base_property_accepted).
//
// §12.3: `base.P` on a CONCRETE (virtual) base property is valid and
// must not be rejected — the override-bypass is exactly the §12.3
// contract.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class B public virtual property P: int => 100; end B; class D: B public override property P: int => 7; public method ViaBaseProp(): int return base.P; end ViaBaseProp; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
