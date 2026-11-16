//! case: s14_2_4915
//! doc: spec
//! section: 14.2
//! title: a static interface implementation omitting where rejects string at the call
//! rule: Omitting the `where` clauses is the recommended spelling and means exactly the inherited set ... dispatch flows through the base or interface signature, so its constraints are the only ones call sites are ever checked against
//! cites: 14.1, 11.2
//! expect: reject
//! diagnostic: does not satisfy the constraint
//! issue: 8649
module Conf;
interface IMaker
  static method Make(): int;
end IMaker;
interface I
  static method Z<T>(): int where T: IMaker;
end I;
class D: I
  public static method Z<T>(): int => 5;
end D;
class P
  public static method Main(): int
    Console.WriteLine(D.Z<string>());
    return 0;
  end Main;
end P;
