//! case: s14_1_0008
//! doc: spec
//! section: 14.1
//! title: an inherited generic indexer's argument is checked against the substituted type
//! rule: An instantiation of a generic declaration is the constructed type or
//!   constructed member obtained by substituting type arguments for its type
//!   parameters; its site is the program point at which the program forms one.
//!   The base clause `Root<Array<U>>` at `Mid<int>` forms the inherited
//!   indexer with its parameter type substituted to Array<int>, and a value of
//!   one type implicitly converts to another when no programmer-visible
//!   information is lost — no string to Array<int> conversion does.
//! cites: 14.1, 5.1
//! expect: reject
//! diagnostic: not implicitly convertible
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
    var v := m["not an array"];
    Console.WriteLine(v);
    return 0;
  end Main;
end Program;
