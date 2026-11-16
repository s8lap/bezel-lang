//! case: s14_2_4905
//! doc: spec
//! section: 14.2
//! title: an override restating where T: class rejects an int argument at the call
//! rule: Non-nullable reference types only
//! cites: 14.1
//! expect: reject
//! diagnostic: constraint
//! issue: 8649
module Conf;
class B
  public virtual method Z<T>(a: T): int where T: class => 1;
end B;
class D: B
  public constructor()
  end constructor;
  public override method Z<T>(a: T): int where T: class => 2;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(5));
    return 0;
  end Main;
end P;
