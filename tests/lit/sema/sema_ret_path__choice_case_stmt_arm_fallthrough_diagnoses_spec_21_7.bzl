// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2129 (§21.7): choice case-statement totality also requires that EVERY arm
// diverges. The match below is exhaustive over `R` (both cases covered), but the
// `Error` arm does NOT diverge — it runs a statement and falls off the end of
// the arm, so control reaches the method end with no return. §21.7 must fire.
// This pins condition (a) of the totality rule (all arms diverge), independent
// of the exhaustiveness condition (b).
//
// Cite spec §21.7, §19.6 (v0.217).

module M;
choice R Ok(v: int), Error(m: int); end R;
class C
  public method Classify(r: R): int
    case r of
      Ok(var v) => return v;,
      Error(_)  => do Console.WriteLine("err"); end
    end;
  end Classify;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `Classify` has a return type but does not return a value on every path
// CHECK-SAME: (§21.7)
