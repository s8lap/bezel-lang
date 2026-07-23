// #2051 — §21.5 / §31.2 (v0.182 rule generalized to labeled targets in v0.201):
// inside a `finally`, a `break` — labeled or not — whose target loop encloses
// the `finally` is a compile error (it would abandon an in-flight exception).
// Here `break scan;` in the finally targets the enclosing labeled `for`.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main()
  scan: for var i in 0..2 do
    try
      Console.WriteLine("x");
    finally
      break scan;
    end;
  end scan;
end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `break` inside a `finally` block may not target a loop that encloses the `finally` (§21.5/§31.2)
