// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §17.15 / §28.3 v0.184 (#1958; #2098 review F1): a closure inside a
// binding atom's CLAUSE INITIALIZER that consumes a captured owned binding —
// an EARLIER clause's or an outer local's — is rejected exactly as one in
// the atom's body or at statement level. Without the per-clause scan this
// was silently accepted: the closure's dispose plus the K8.5 boundary /
// scope-end disposal double-freed (the CONFIRMED review finding).
module M;
class Res
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor Console.WriteLine("D"); end destructor;
end Res;
class W
  public static method Sink(owned r: Res): int
    var v := r.Id;
    r.Dispose();
    return v;
  end Sink;
  public static method Run(f: () -> ()): int
    f();
    return 0;
  end Run;
end W;
class C
  // (1) The closure consumes an EARLIER owned clause of the same atom.
  // §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.
  public static method A(): int
    var v := (var c := new Res(1); var k := W.Run(lambda() => do _ := W.Sink(move c); end); k + c.Id);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot transfer captured owned binding `c` inside a closure
    // CHECK-SAME: (§28.3, v0.184)
    return v;
  end A;

  // (2) The closure consumes an OUTER owned local from a clause initializer.
  // §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.
  public static method B(): int
    var outer := new Res(2);
    var v := (var k := W.Run(lambda() => do _ := W.Sink(move outer); end); k);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot transfer captured owned binding `outer` inside a closure
    outer.Dispose();
    return v;
  end B;
end C;
