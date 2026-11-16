//! case: s14_2_4916
//! doc: spec
//! section: 14.2
//! title: a static interface implementation omitting where admits a conforming type; the body reads the bound
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
  public static method Make(): int => 42;
end K;
interface I
  static method Z<T>(): int where T: IMaker;
end I;
class D: I
  public static method Z<T>(): int => T.Make();
end D;
class P
  public static method Main(): int
    Console.WriteLine(D.Z<K>());
    return 0;
  end Main;
end P;
