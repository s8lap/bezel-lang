//! case: s23_4_8935
//! doc: spec
//! section: 23.4
//! title: an interface indexer call selects the overload with the matching function return type
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 27.1, 11.2
//! expect: run
//! output: 2
//! notes: Near-miss overload selection (#9137 review, N1). The class declares `[q: () -> int]` (1) and `[q:
//!   () -> string]` (2); the interface member is `[q: () -> string]`, so the interface slot is the
//!   second: 2.
module Conf;
public interface IFl
  readonly property [q: () -> string]: int;
end IFl;

public class FlI : IFl
  public property [q: () -> int]: int => 1;
  public property [q: () -> string]: int => 2;
  public constructor() end constructor;
end FlI;

public class Program
  public static method Main(): int
    using var c := new FlI();
    var fl: IFl := c;
    Console.WriteLine($"{fl[lambda() => "a"]}");
    return 0;
  end Main;
end Program;
