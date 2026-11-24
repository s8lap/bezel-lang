//! case: s17_6_8402
//! doc: spec
//! section: 17.6
//! title: a compound indexer assignment whose getter returns a closure's result accumulates
//! rule: The indexer's getter and setter are each called once.
//! cites: 17.6, 23.4, 27.5
//! expect: run
//! stdout: 40600
//! notes: Pins #9065 (closure returned by a closure in both accessors). Getter: `mk()` yields a
//!   closure returning i + 1, so the read is `(i + 1) + (n mod 5)`; setter: `n := n + (i +
//!   value)`. `h[i] += 1` run for i = 0..199 leaves n = 40600.
module Conf;
import System;
public class H
  public n: int;
  public property [i: int]: int
    get => (var mk := lambda(): () -> int => lambda(): int => i + 1; var f := mk(); var z := f(); z + (n mod 5));
    set => do
      var mk := lambda(): () -> int => lambda(): int => i + value;
      var f := mk();
      n := n + f();
    end;
  end;
end H;
public class Program
  public static method Main(): int
    using var h := new H();
    var i := 0;
    while i < 200 do
      h[i] += 1;
      i := i + 1;
    end while;
    Console.Write(h.n);
    return 0;
  end Main;
end Program;
