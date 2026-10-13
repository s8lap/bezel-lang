//! case: s11_2_0070
//! doc: spec
//! section: 11.2
//! title: a class type parameter substituted beside an own type parameter matches
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. The interface's class parameter S is substituted by the implementing class's Z, and the own parameter U corresponds to W by position, so Box<S>, Box<U> matches Box<Z>, Box<W>.
//! cites: 11.2
//! expect: run
//! stdout: 12
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I<S>
  method P<U>(a: Box<S>, b: Box<U>): int;
end I;

public class K<Z>: I<Z>
  public constructor() end constructor;

  public method P<W>(a: Box<Z>, b: Box<W>): int
    return 12;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I<int> := new K<int>();
    Console.WriteLine(s.P<string>(new Box<int>(), new Box<string>()));
    return 0;
  end Main;
end C;
