//! case: s11_2_0116
//! doc: spec
//! section: 11.2
//! title: a property typed by the interface's parameter matches at the substituted type
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   Under I<int> the property type T is int, and the implementation's property of type int is exactly equal to it.
//! cites: 11.2
//! expect: run
//! stdout: 9
module Conf;

public interface I<T>
  readonly property Value: T;
end I;

public class K: I<int>
  public constructor() end constructor;

  public property Value: int := 9;
end K;

public class C
  public static method Main(): int
    var s: I<int> := new K();
    Console.WriteLine(s.Value);
    return 0;
  end Main;
end C;
