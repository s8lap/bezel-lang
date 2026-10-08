//! case: s15_1_0007
//! doc: spec
//! section: 15.1
//! title: a user generic storing T? named at an inline array is rejected at the instantiation site
//! rule: a type `T?` — a field, local, parameter or result, a payload or
//!   element type, a type argument, or a local the body declares — at a type
//!   parameter `T` substituted by an inline array is a compile error at the
//!   instantiation site (§3.6 rule 2), through any chain of user declarations
//! cites: 15.1, 3.6, 7.10
//! expect: reject
//! diagnostic: nullable inline-array
//! issue: 5641
module Conf;

public class Box<T>
  private _v: T? := null;

  public constructor()
  end constructor;
end Box;

public class Program
  public static method Main()
    using var b := new Box<[3]int>();
  end Main;
end Program;
