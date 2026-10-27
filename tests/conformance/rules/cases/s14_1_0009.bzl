//! case: s14_1_0009
//! doc: spec
//! section: 14.1
//! title: the same inherited indexer accepts an argument of the substituted type
//! rule: An instantiation of a generic declaration is the constructed type or
//!   constructed member obtained by substituting type arguments for its type
//!   parameters; its site is the program point at which the program forms one.
//!   The base clause `Root<Array<U>>` at `Mid<int>` forms the inherited
//!   indexer with its parameter type substituted to Array<int>, and an
//!   Array<int> argument converts to it without loss, so the inherited
//!   member's body is the one that answers.
//! cites: 14.1, 5.1
//! expect: run
//! stdout: root
module Conf;

public class Root<T>
  public constructor() end constructor;
  public virtual property [i: T]: string => "root";
end Root;

public class Mid<U>: Root<Array<U>>
  public constructor() end constructor;
end Mid;

public class Program
  public static method Main(): int
    using var m := new Mid<int>();
    const arr: Array<int> := [7, 8];
    Console.WriteLine(m[arr]);
    return 0;
  end Main;
end Program;
