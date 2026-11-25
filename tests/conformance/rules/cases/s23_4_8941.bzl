//! case: s23_4_8941
//! doc: spec
//! section: 23.4
//! title: an interface indexer with a unit-returning function-typed parameter dispatches to the
//!   implementation
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 11.2, 27.1
//! expect: run
//! output: 5
//! issue: 9137
//! notes: Known divergence (#9137, still open after the `narrow #9137` fix `3fd82a439`; review
//!   r43-rev-m64-9137 finding F1). Same shape as 8931 but the callback type is `() -> ()`. The spec
//!   requires the interface call to reach the implementation (5); the compiler still crashes.
module Conf;
public interface IFl
  readonly property [q: () -> ()]: int;
end IFl;

public class FlI : IFl
  public property [q: () -> ()]: int => 5;
  public constructor() end constructor;
end FlI;

public class Program
  public static method Main(): int
    using var c := new FlI();
    var fl: IFl := c;
    Console.WriteLine($"{fl[lambda() => do end]}");
    return 0;
  end Main;
end Program;
