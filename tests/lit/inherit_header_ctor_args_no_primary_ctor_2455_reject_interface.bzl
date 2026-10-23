// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #2455 (Appendix A `InheritItem` grammar note, v0.235): the grammar
// comment on `InheritItem ::= Type ("(" ArgList? ")")?` states the argument
// list "is permitted only on the base class (the first item), not on
// interfaces" — previously unenforced (silently tokenized away, same
// shape as the base-position bug). A NON-EMPTY argument list on an
// interface position (second-or-later inheritance item) is now rejected.
//
// #7069: this row rejects at PARSE, so the front end does not run on it at
// all and the ICE-escape guard below is defence-in-depth, not a live
// assertion: a parser crash prints no diagnostic, and the positive content
// match is what turns the row red. The guard is kept (and placed where a
// front-end marker would land) so that a future change that lets the front
// end run on this shape is still covered.

module M;

public interface IMarker
end IMarker;

public class Base
end Base;

public class Derived: Base, IMarker(1)
end Derived;

// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// #7069: the front end's ICE marker is printed BEFORE any user-facing
// diagnostic, and a negative directive scans only the region between the
// surrounding matches — below the positive match this needle could never
// fire, so it is written above it, where it can.
// CHECK-NOT: internal compiler error
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: an argument list is not permitted here
// CHECK-SAME: permitted only on the base class
// CHECK-SAME: not on an interface
