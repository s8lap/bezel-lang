//! case: s14_2_4902
//! doc: spec
//! section: 14.2
//! title: an interface implementation restating where T: INumber<T> rejects a string argument at the call
//! rule: Non-nullable types conforming to `IFoo`: classes implementing it, records and enums implementing it, interfaces extending it
//! cites: 14.1, 11.2
//! expect: reject
//! diagnostic: constraint
//! issue: 8649
module Conf;
interface I
  method Z<T>(a: T): T where T: INumber<T>;
end I;
class D: I
  public method Z<T>(a: T): T where T: INumber<T> => T.Zero();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z("x"));
    return 0;
  end Main;
end P;
