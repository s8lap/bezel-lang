// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3641 (§12.5 "Abstract classes and members", §23.4 "Indexers", v0.286):
// the indexer sibling of
// sema_abs_impl__unimplemented_abstract_method_diagnoses.bzl. A concrete
// class inheriting, without overriding, an abstract CLASS indexer must be
// rejected — §12.5: "`abstract` on a member declares it without a body;
// every concrete subclass must override it"; §23.4 states an indexer
// "participate[s] in `virtual`/`override`/`sealed`/`abstract` like methods",
// with no carve-out excluding indexers from the §12.5 override obligation.
//
// Before this fix, TypeResolution_Checks.cpp's abstract-member-
// implementation walk tracked abstract METHOD/PROPERTY obligations
// (`abstractNames`) but unconditionally `continue`d past every indexer
// regardless of whether the declaring ancestor was a class or an interface
// — only the INTERFACE half was checked (`ifaceIndexerObligations`, gated
// `if (curIsIface)`). This shape compiled with zero diagnostics and the
// resulting program SIGSEGV'd at the first indexer access, because
// codegen's vtable slot for the indexer was never filled (issue #3641).

module M; abstract class Base public abstract property [i: int]: int; end Base; class D : Base end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: concrete class `D` does not implement inherited abstract member `indexer`
// CHECK-SAME: (§12.5)
