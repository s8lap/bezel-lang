// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #2455 (Appendix A `InheritItem` grammar note, v0.235): the grammar
// comment on `InheritItem ::= Type ("(" ArgList? ")")?` states the argument
// list "is permitted only on the base class (the first item), not on
// interfaces" — previously unenforced (silently tokenized away, same
// shape as the base-position bug). A NON-EMPTY argument list on an
// interface position (second-or-later inheritance item) is now rejected.

module M;

public interface IMarker
end IMarker;

public class Base
end Base;

public class Derived: Base, IMarker(1)
end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: an argument list is not permitted here
// CHECK-SAME: permitted only on the base class
// CHECK-SAME: not on an interface
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
