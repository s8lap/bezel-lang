//! case: s23_4_8938
//! doc: spec
//! section: 23.4
//! title: an interface indexer call selects the overload whose nested function type matches
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 27.1, 11.2
//! expect: run
//! output: 2
//! notes: Near-miss overload selection (#9137 review, N9). A function type whose parameter is itself a
//!   function type, differing in the nested parameter type: 2.
module Conf;
public interface IFl
  readonly property [q: ((int) -> int) -> int]: int;
end IFl;

public class FlI : IFl
  public property [q: ((string) -> int) -> int]: int => 1;
  public property [q: ((int) -> int) -> int]: int => 2;
  public constructor() end constructor;
end FlI;

public class Program
  public static method Main(): int
    using var c := new FlI();
    var fl: IFl := c;
    Console.WriteLine($"{fl[lambda(f: (int) -> int) => f(1)]}");
    return 0;
  end Main;
end Program;
