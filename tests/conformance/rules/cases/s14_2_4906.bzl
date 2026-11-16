//! case: s14_2_4906
//! doc: spec
//! section: 14.2
//! title: a static interface implementation restating where T: IMaker rejects string at the call
//! rule: Non-nullable types conforming to `IFoo`: classes implementing it, records and enums implementing it, interfaces extending it
//! cites: 14.1, 11.8
//! expect: reject
//! diagnostic: constraint
//! issue: 8649
module Conf;
interface IMaker
  static method Make(): int;
end IMaker;
interface I
  static method Z<T>(): int where T: IMaker;
end I;
class D: I
  public static method Z<T>(): int where T: IMaker => 5;
end D;
class P
  public static method Main(): int
    Console.WriteLine(D.Z<string>());
    return 0;
  end Main;
end P;
