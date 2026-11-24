//! case: s27_5_8305
//! doc: spec
//! section: 27.5
//! title: a lambda in an indexer getter under h[i] mod= 7 reads the index of its own call
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 6.6, 23.4
//! expect: run
//! stdout: 606
//! notes: Pins #8941 (the `mod=` compound operator). The getter returns v = i + 3 and the
//!   stored value is v mod 7, with v running over 3..202. Hand sum: v = 3..6 gives 3 + 4 +
//!   5 + 6 = 18; v = 7..202 is 196 values = 28 full residue cycles of sum 21 = 588; total
//!   606.
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
    while i < 200 do h[i] mod= 7; i := i + 1; end;
    Console.WriteLine(h.n);
    return 0;
  end Main;
end Program;
