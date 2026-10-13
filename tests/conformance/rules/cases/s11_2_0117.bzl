//! case: s11_2_0117
//! doc: spec
//! section: 11.2
//! title: a method's return type substituted to string matches the implementation's string return
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   The return type is compared under the interface's substitution: under I<string> the return type T is string, which the implementation's return type string is exactly equal to.
//! cites: 11.2
//! expect: run
//! stdout: hi
module Conf;

public interface I<T>
  method Get(): T;
end I;

public class K: I<string>
  public constructor() end constructor;

  public method Get(): string
    return "hi";
  end Get;
end K;

public class C
  public static method Main(): int
    var s: I<string> := new K();
    Console.WriteLine(s.Get());
    return 0;
  end Main;
end C;
