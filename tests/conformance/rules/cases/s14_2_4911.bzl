//! case: s14_2_4911
//! doc: spec
//! section: 14.2
//! title: an override of a base method implementing an interface rejects an argument outside its bound
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
interface I
  method Z<T>(a: T): int where T: IMaker;
end I;
class B: I
  public constructor()
  end constructor;
  public virtual method Z<T>(a: T): int where T: IMaker => T.Make();
end B;
class D: B
  public override method Z<T>(a: T): int => T.Make();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(new Q()));
    var i: I := d;
    Console.WriteLine(i.Z(new Q()));
    return 0;
  end Main;
end P;
