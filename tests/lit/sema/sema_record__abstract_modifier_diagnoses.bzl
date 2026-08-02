// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #2431 (§9.6 "Records and inheritance" / §12.5 "Abstract classes and
// members"): `abstract record` was admitted by the parser (which
// deliberately over-admits modifier combinations and defers validity to
// sema) but nothing in sema rejected it, and nothing defines what it would
// mean. §12.5 defines `abstract` only in terms of a class ("`abstract` on a
// class permits abstract members and forbids direct instantiation"); §9.6
// states records are implicitly sealed and can never be derived from
// (neither by another record nor by a class), so an `abstract record`
// could never have a concrete subclass through any route to provide
// bodies for its abstract members. This is a decline-with-reasons
// interpretation (CLAUDE.md rule 4): the spec never explicitly forbids the
// token sequence, it simply never defines a meaning for it, and rejecting
// is the chosen, recorded reading (see issue #2431).

module M; public abstract record R(X: int);
public class C
  public static method Main(): int
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` may not be `abstract`
// CHECK-SAME: (§9.6)
// CHECK-SAME: (§12.5)
