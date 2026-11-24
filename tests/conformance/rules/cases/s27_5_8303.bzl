//! case: s27_5_8303
//! doc: spec
//! section: 27.5
//! title: a lambda in an indexer getter with two index parameters reads both of its own call
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 6.6, 23.4
//! expect: run
//! stdout: 200394
//! notes: Pins #8941 (two index parameters). Per pass the getter returns 10i + (i mod 7) + 3,
//!   so the stored value is 10i + (i mod 7) + 4. Hand sums over i = 0..199: 10 * 19900 =
//!   199000; i mod 7 sums to 28 * 21 + (0 + 1 + 2 + 3) = 594; 4 * 200 = 800; total 200394.
module Conf;
import System;
public class H
  public n: int;
  public property [i: int, j: int]: int
    get => (var g := lambda(): int => 3; var q := g(); var k := lambda(): int => i * 10 + j + q; k());
    set => n := n + value;
  end;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var i := 0;
    while i < 200 do h[i, i mod 7] += 1; i := i + 1; end;
    Console.WriteLine(h.n);
    return 0;
  end Main;
end Program;
