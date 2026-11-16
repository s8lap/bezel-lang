//! case: s14_2_4907
//! doc: spec
//! section: 14.2
//! title: a three-level chain of overrides omitting where rejects an argument outside the root's bound
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
class A
  public virtual method Z<S>(a: S): int where S: IMaker => 1;
end A;
class B: A
  public override method Z<S>(a: S): int => 2;
end B;
class C: B
  public override method Z<S>(a: S): int => 3;
end C;
class D: C
  public constructor()
  end constructor;
  public override method Z<R>(a: R): int => R.Make();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(new Q()));
    return 0;
  end Main;
end P;
