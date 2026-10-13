//! case: s11_2_0100
//! doc: spec
//! section: 11.2
//! title: an inline array nested in a generic argument matches its substituted concrete form
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   The interface's substitution maps S to int, so Box<[3]S> under I<int> is Box<[3]int>, which the implementation's parameter Box<[3]int> is exactly equal to.
//! cites: 11.2
//! expect: run
//! stdout: 4
//! issue: 6088
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I<S>
  method P(a: Box<[3]S>): int;
end I;

public class K: I<int>
  public constructor() end constructor;

  public method P(a: Box<[3]int>): int
    return 4;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I<int> := new K();
    Console.WriteLine(s.P(new Box<[3]int>()));
    return 0;
  end Main;
end C;
