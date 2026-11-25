//! case: s23_4_8940
//! doc: spec
//! section: 23.4
//! title: two interfaces implemented by one class each reach their own function-typed indexer
//! rule: Indexers may be overloaded by parameter type list.
//! cites: 23.4, 11.2
//! expect: run
//! output: 101
//!   205
//! notes: Pins #9137 (K3). `IA` declares `[q: () -> int]`, `IB` declares `[q: (int) -> int]`; the class
//!   implements both, so each interface slot reaches its own indexer: 1 + 100 = 101 and 5 + 200 = 205.
module Conf;
public interface IA
  readonly property [q: () -> int]: int;
end IA;

public interface IB
  readonly property [q: (int) -> int]: int;
end IB;

public class C : IA, IB
  public property [q: () -> int]: int => q() + 100;
  public property [q: (int) -> int]: int => q(5) + 200;
  public constructor() end constructor;
end C;

public class Program
  public static method Main(): int
    using var c := new C();
    var a: IA := c;
    var b: IB := c;
    Console.WriteLine($"{a[lambda() => 1]}");
    Console.WriteLine($"{b[lambda(x: int) => x]}");
    return 0;
  end Main;
end Program;
