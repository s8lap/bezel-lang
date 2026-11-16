//! case: s14_2_4914
//! doc: spec
//! section: 14.2
//! title: an implementation omitting where reads the bound I<IMaker> substitutes and admits a K
//! rule: Omitting the `where` clauses is the recommended spelling and means exactly the inherited set
//! cites: 14.1, 11.2
//! expect: run
//! output: 42
//! issue: 8618
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
    Console.WriteLine(d.M(new K()));
    return 0;
  end Main;
end P;
