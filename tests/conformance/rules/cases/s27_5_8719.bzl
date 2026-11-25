//! case: s27_5_8719
//! doc: spec
//! section: 27.5
//! title: a record indexer read with a lambda index argument that captures the record
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 23.4, 9.4
//! expect: run
//! stdout: 800
//! notes: Pins #9099 (record receiver). The record `Pr(a: 1, b: 2)`'s read-only indexer returns
//!   `q() + a + b` with q() = p.a = 1: 1 + 1 + 2 = 4, 200 times: 800.
module Conf;
public record Pr(a: int, b: int)
  public property [q: () -> int]: int => q() + a + b;
end Pr;
public class Program
  public static method Main(): int
    var t := 0;
    var p := new Pr(1, 2);
    for var j in 0 ..< 200 do
      t := t + p[lambda() => p.a];
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
