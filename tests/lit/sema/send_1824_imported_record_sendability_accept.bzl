// RUN: %bezelc -c -o %t.dep.o %S/send_1824_imported_record_sendability_lib.bzl
// RUN: %bezelc -c -o %t.o %s --import %t.dep.o
// RUN: %bezelc -c %s --import %t.dep.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #1824 — the over-reject guard for the sibling reject test. The #1824
// fix must NOT flip to rejecting EVERY imported record: a genuinely-sendable
// imported record must still be accepted across a domain boundary. Spec §2.8.2
// clause 1 / §30.14, v0.196.
//
// `ValArg` (imported, only value-typed fields) and `NestVal` (imported, field
// of the sendable `ValArg` — recursion bottoms out sendable) are both sendable,
// so each `Channel<T>` element position is accepted and the importer compiles
// clean (exit zero, no diagnostics). The first RUN line builds the RecLib
// artifact; the second is the clean-compile accept; the third asserts no
// diagnostics.
module App;
import RecLib;

class C
  public static method Main(): int
    using var a := new Channel<ValArg>();
    using var b := new Channel<NestVal>();
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
