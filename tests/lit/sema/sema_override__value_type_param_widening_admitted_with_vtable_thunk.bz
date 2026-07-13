// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, value_type_param_widening_admitted_with_vtable_thunk).
//
// §12.1 (v0.181, #1560/#1592): a VALUE-typed parameter (`int`) widening is
// now ADMITTED, closing the #1592 follow-up filed alongside #1560. Unlike a
// reference type (class/interface/string/Object), whose `T` and `T?` forms
// share one runtime representation, a nullable value type lowers to a
// distinct boxed `{i8 hasValue, T}` representation — but codegen now
// installs a vtable adapter thunk (mirroring the itable case's
// ensureNullableWideningThunk, #1139/#1183) whenever a value-typed parameter
// is widened somewhere in a virtual slot's hierarchy, so a virtual call
// built from a non-widened ancestor's static type marshals correctly instead
// of corrupting (see LlvmBackend_Setup.cpp's introducingVirtualMethod /
// widenedParamPositions / ensureVtableWideningThunk and the codegen
// regression tests in tests/codegen/virtual_dispatch_test.cpp).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public virtual method F(a: int) _ := 1; end F; end Base; class D : Base public override method F(a: int?) _ := 2; end F; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
