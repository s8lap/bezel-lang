// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §17.15 / §28.3 / K8.4-at-the-atom-boundary, v0.216 (impl #2098):
// "A borrow whose source is an atom binding may not escape the atom — in the
// body's value or any other holder — per K8.4: rejected, never
// lifetime-extended; bind at statement level (`using var`, §28.10) when the
// value must outlive the expression." And: "The body is NOT among K3.4's
// marked-transfer positions in this revision (an atom cannot transfer its
// own binding out as its result)."
module M;
class Res
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor Console.WriteLine("D"); end destructor;
end Res;
class C
  // (1) The bare-identifier body: the atom's value would be a borrow of the
  // owned binding `w`, disposed at the atom's completion — dangling.
  public static method A(): int
    var r := (var w := new Res(1); w);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the binding atom's body value borrows atom binding `w`
    // CHECK-SAME: may not escape the atom
    // CHECK-SAME: (K8.4 at the atom boundary, §17.15 / §28.3, v0.216)
    return r.Id;
  end A;

  // (2) Transitive through a borrow clause (K2.2 source carriage): `q`
  // carries `c`'s source, and `c` is an owned atom binding.
  public static method B(): int
    var r := (var c := new Res(2); var q := c; q);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the binding atom's body value borrows atom binding `c`
    return r.Id;
  end B;

  // (3) Capture in an escaping closure: the body-value lambda captures the
  // owned binding — the closure would outlive the atom while its captured
  // borrow's source is disposed at the atom's completion.
  public static method D(): int
    var f := (var c := new Res(3); lambda(): int => c.Id);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: this closure captures binding-atom binding `c`
    // CHECK-SAME: (K8.4 at the atom boundary, §17.15 / §28.3, v0.216)
    return f();
  end D;

  // (4) The `=>` expression-body position — the body IS the return value;
  // the atom-level reject applies identically (the ownership dataflow does
  // not run on `=>` bodies; the self-contained escape check does).
  public static method E(): owned Res => (var c := new Res(4); c);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the binding atom's body value borrows atom binding `c`

  // (5) The atom does not LAUNDER an enclosing borrow past the return
  // checks: the atom's value is its body's, so returning an atom whose body
  // reads a `using var` is the §28.3 borrow-lifetime escape, exactly as
  // `return u;`/`return u.M()` are.
  public static method F(): owned Res
    using var u := new Res(5);
    return (var n := 1; u);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: borrow returned from `u` would outlive its source
    // CHECK-SAME: (§28.3 borrow-lifetime rule
  end F;
end C;
