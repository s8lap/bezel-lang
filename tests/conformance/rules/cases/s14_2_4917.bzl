//! case: s14_2_4917
//! doc: spec
//! section: 14.2
//! title: an override omitting where inherits the INumber bound and rejects a string argument at the call
//! rule: Omitting the `where` clauses is the recommended spelling and means exactly the inherited set ... dispatch flows through the base or interface signature, so its constraints are the only ones call sites are ever checked against
//! cites: 14.1, 11.2
//! expect: reject
//! diagnostic: does not satisfy the constraint
//! issue: 8649
module Conf;
class B
  public virtual method Z<T>(a: T): T where T: INumber<T> => a + a;
end B;
class D: B
  public override method Z<T>(a: T): T => a + a;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z("x"));
    return 0;
  end Main;
end P;
