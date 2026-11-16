//! case: s14_2_4920
//! doc: spec
//! section: 14.2
//! title: an override omitting where T: value inherits the kind constraint and rejects a class at the call
//! rule: Omitting the `where` clauses is the recommended spelling and means exactly the inherited set ... dispatch flows through the base or interface signature, so its constraints are the only ones call sites are ever checked against
//! cites: 14.1, 11.2
//! expect: reject
//! diagnostic: does not satisfy the constraint
//! issue: 8649
module Conf;
interface IMaker
  static method Make(): int;
end IMaker;
class K: IMaker
  public constructor()
  end constructor;
  public static method Make(): int => 42;
end K;
class Q
  public constructor()
  end constructor;
end Q;
class B
  public virtual method Z<T>(a: T): int where T: value => 1;
end B;
class D: B
  public constructor()
  end constructor;
  public override method Z<T>(a: T): int => 2;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(new Q()));
    return 0;
  end Main;
end P;
