//! case: s23_4_8942
//! doc: spec
//! section: 23.4
//! title: an interface indexer with a unit-returning function-typed parameter taking an int dispatches
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 11.2, 27.1
//! expect: run
//! output: 6
//! issue: 9137
//! notes: Known divergence (#9137). Same as 8941 with an `(int) -> ()` callback; expected 6, the compiler
//!   crashes.
module Conf;
public interface IFl
  readonly property [q: (int) -> ()]: int;
end IFl;

public class FlI : IFl
  public property [q: (int) -> ()]: int => 6;
  public constructor() end constructor;
end FlI;

public class Program
  public static method Main(): int
    using var c := new FlI();
    var fl: IFl := c;
    Console.WriteLine($"{fl[lambda(x: int) => do end]}");
    return 0;
  end Main;
end Program;
