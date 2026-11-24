//! case: s27_5_8306
//! doc: spec
//! section: 27.5
//! title: a lambda in an indexer getter under h[i] ^= 7 reads the index of its own call
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 6.6, 23.4
//! expect: run
//! stdout: 20500
//! notes: Pins #8941 (the `^=` compound operator). The getter returns v = i + 3 and the stored
//!   value is v xor 7, with v running over 3..202. Hand sum: v = 3..7 gives 4 + 3 + 2 + 1
//!   + 0 = 10; v = 8..199 is 24 aligned blocks of 8 that xor 7 permutes within, so their
//!   sum is unchanged, (8 + 199) * 192 / 2 = 19872; v = 200..202 gives 207 + 206 + 205 =
//!   618; total 20500.
module Conf;
import System;
public class H
  public n: int;
  public property [i: int]: int
    get => (var g := lambda(): int => 3; var q := g(); var k := lambda(): int => i + q; k());
    set => n := n + value;
  end;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var i := 0;
    while i < 200 do h[i] ^= 7; i := i + 1; end;
    Console.WriteLine(h.n);
    return 0;
  end Main;
end Program;
