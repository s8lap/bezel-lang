//! case: s14_2_4912
//! doc: spec
//! section: 14.2
//! title: an override of a base method implementing an interface admits a conforming argument on both calls
//! rule: Omitting the `where` clauses is the recommended spelling and means exactly the inherited set
//! cites: 14.1, 11.2
//! expect: run
//! output: 42
//!   42
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
    Console.WriteLine(d.Z(new K()));
    var i: I := d;
    Console.WriteLine(i.Z(new K()));
    return 0;
  end Main;
end P;
