// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=CHECK %s
//
// fix-forward #3575 (§35.4 / §35.3 / §28.3 K3.14, v0.334): the POSITIVE half of
// the rule, and the ratchet for the producerState arm this commit corrects.
//
// §35.3: "an owning construction is an `owned` move-only value … the caller
// takes ownership and discharges by binding, storing, returning, or consuming
// it". §35.4: "locals are auto-disposed at scope end with the must-use warning
// (K3.14)". So `var b := Bundle.Ready(r)` is Owned, and one that is never
// discharged — bound and then only read — must draw the §28.3 must-use
// warning.
//
// This file is what keeps the corrected arm HONEST. The defect being fixed
// over-classified as Owned, so removing the arm outright also makes this file
// pass; only the POSITIVE test (`sema::ChoiceCaseSymbol` whose parent is a
// choice) keeps it firing on the case construction it exists to catch. A
// `ConstructorSymbol` test does not: that symbol is a SIBLING of
// `ChoiceCaseSymbol` under `MemberSymbol`, so the cast is unconditionally
// null and this file goes green with the rule retired. MEASURED.
//
// The lit track is where this oracle belongs: `runAot` carries `diagnostics`
// only on a reject, so a warning on a program that COMPILES is invisible to a
// gtest row. The program here compiles — that is the point, the warning is not
// a rejection — and the sibling row
// `owning_choice_case_ctor_only_is_a_fresh_producer_3575_test.cpp` pins the
// matching disposal behaviour.

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: warning: owned value in `b` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 must-use / auto-drop)
// A must-use warning is not a rejection: nothing here may be an Error, or the
// §35.4 sentence's own remedy (`using var`) would be unreachable.
// CHECK-NOT: error

module M;
class Resource
  private _n: int;
  public constructor(n: int) _n := n; end constructor;
  public method N(): int => _n;
end Resource;
choice Bundle
  Ready(owned r: Resource),
  Empty;
end Bundle;
class C
  public static method Main(): int
    var b := Bundle.Ready(new Resource(42));   // a CONSTRUCTION: owns the payload
    Console.WriteLine("body");
    return 0;
  end Main;
end C;