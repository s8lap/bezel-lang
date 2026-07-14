// RUN: %bezelc -c -o %t.dep.o %S/send_1732_imported_class_sendability_lib.bzl
// RUN: %bezelc -c -o %t.o %s --import %t.dep.o
// RUN: %bezelc -c %s --import %t.dep.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #1732 — the over-reject guard for the sibling reject test. The #1732
// fix must NOT flip to rejecting EVERY imported class: a genuinely-sendable
// imported class must still be accepted across a domain boundary. Spec §2.8.2
// clause 1 / §30.14, v0.196.
//
// `Point` (imported, only value-typed fields) and `OwnsSendable` (imported,
// `owned` field of the sendable `Point`) are both sendable, so each
// `Channel<T>` element position is accepted and the importer compiles clean
// (exit zero, no diagnostics). The first RUN line builds the SendLib artifact;
// the second is the clean-compile accept; the third asserts no diagnostics.
module App;
import SendLib;

class C
  public static method Main(): int
    using var a := new Channel<Point>();
    using var b := new Channel<OwnsSendable>();
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
