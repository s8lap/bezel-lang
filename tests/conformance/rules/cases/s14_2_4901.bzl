//! case: s14_2_4901
//! doc: spec
//! section: 14.2
//! title: a class override restating where T: INumber<T> rejects a string argument at the call
//! rule: Non-nullable types conforming to `IFoo`: classes implementing it, records and enums implementing it, interfaces extending it
//! cites: 14.1
//! expect: reject
//! diagnostic: constraint
//! issue: 8649
module Conf;
class B
  public virtual method Z<T>(a: T): T where T: INumber<T> => T.Zero();
end B;
class D: B
  public override method Z<T>(a: T): T where T: INumber<T> => T.Zero();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z("x"));
    return 0;
  end Main;
end P;
