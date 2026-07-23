// #2051 — §21.5 (v0.201): the enclosing-loop search is confined to the callable
// whose body contains the jump. A loop label of the defining method is NOT in
// scope inside a lambda, so `break scan;` inside a lambda cannot target the
// method's `scan` loop — a compile error naming the (empty) label scope.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main()
  scan: for var i in 0..2 do
    var f := lambda() => do
      for var j in 0..2 do
        break scan;
      end;
    end;
    f();
  end scan;
end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unknown loop label 'scan' in `break` (§21.5); no loop labels are in scope
