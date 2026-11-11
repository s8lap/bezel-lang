//! case: s14_2_4502
//! doc: spec
//! section: 14.2
//! title: an override of the constrained overload inherits its bound; the
//!   unconstrained sibling stays so
//! rule: inherits the overridden or implemented declaration's constraints. Omitting
//!   the `where` clauses is the recommended spelling and means exactly the
//!   inherited set
//! cites: 22.6
//! expect: run
//! output: 11
//!   2
//! issue: 8259
module Conf;

class B
  public virtual method S<T>(a: Array<T>, k: int): T where T: INumber<T> => a.Sum();
  public virtual method S<T>(a: Array<T>): int => 0;
end B;

class D: B
  public override method S<T>(a: Array<T>, k: int): T
    return a.Sum();
  end S;
  public override method S<T>(a: Array<T>): int => a.Count;
end D;

class Prog
  public static method Main(): int
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a, 5));
    Console.WriteLine(d.S(a));
    return 0;
  end Main;
end Prog;
