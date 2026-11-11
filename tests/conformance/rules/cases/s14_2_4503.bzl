//! case: s14_2_4503
//! doc: spec
//! section: 14.2
//! title: an override of the unconstrained overload does not inherit its sibling's
//!   bound
//! rule: inherits the overridden or implemented declaration's constraints. Omitting
//!   the `where` clauses is the recommended spelling and means exactly the
//!   inherited set
//! cites: 11.8
//! expect: reject
//! diagnostic: INumber
//! issue: 8259
module Conf;

class B
  public virtual method S<T>(a: Array<T>, k: int): T where T: INumber<T> => a.Sum();
  public virtual method S<T>(a: Array<T>): int => 0;
end B;

class D: B
  public override method S<T>(a: Array<T>): int
    return a.Sum();
  end S;
end D;

class Prog
  public static method Main(): int
    return 0;
  end Main;
end Prog;
