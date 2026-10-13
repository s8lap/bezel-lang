//! case: s11_2_0021
//! doc: spec
//! section: 11.2
//! title: a type alias in a parameter type matches the aliased generic instance
//! rule: A type alias is a pure synonym, so a parameter declared BI matches a parameter declared Box<int>: the interface's BI and the implementation's Box<int> are exactly equal types. Interface P(a: BI) is matched by P(a: Box<int>), and the call through the interface reaches it.
//! cites: 11.2, 27.8
//! expect: run
//! stdout: 9
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public class Pair<L, R>
  public constructor() end constructor;
end Pair;

type BI := Box<int>;

public interface I
  method P(a: BI): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P(a: Box<int>): int
    return 9;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    Console.WriteLine(s.P(new Box<int>()));
    return 0;
  end Main;
end C;
