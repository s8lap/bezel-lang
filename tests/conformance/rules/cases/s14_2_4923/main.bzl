//! case: s14_2_4923
//! doc: spec
//! section: 14.2
//! title: an override of an imported base restating where T: IMaker rejects a non-IMaker class at the call
//! rule: Non-nullable types conforming to `IFoo`: classes implementing it, records and enums implementing it, interfaces extending it
//! cites: 14.1, 2.8
//! expect: reject
//! diagnostic: does not satisfy the constraint
//! issue: 8649
module Conf;
import Dep;
class Q
  public constructor()
  end constructor;
end Q;
class D: B
  public constructor()
  end constructor;
  public override method Z<T>(a: T): int where T: IMaker => 5;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(new Q()));
    return 0;
  end Main;
end P;
