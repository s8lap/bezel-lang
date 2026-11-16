//! case: s14_2_4908
//! doc: spec
//! section: 14.2
//! title: a three-level override chain omitting where admits a conforming argument; the body reads the bound
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
    Console.WriteLine(d.Z(new K()));
    return 0;
  end Main;
end P;
