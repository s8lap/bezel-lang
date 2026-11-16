//! case: s14_2_4913
//! doc: spec
//! section: 14.2
//! title: an implementation omitting where reads the bound I<IMaker> substitutes and rejects a Q at the call
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
interface I<U>
  method M<S>(x: S): int where S: U;
end I;
class D: I<IMaker>
  public constructor()
  end constructor;
  public method M<T>(x: T): int => T.Make();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.M(new Q()));
    return 0;
  end Main;
end P;
