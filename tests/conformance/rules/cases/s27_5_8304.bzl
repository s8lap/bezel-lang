//! case: s27_5_8304
//! doc: spec
//! section: 27.5
//! title: a lambda in an indexer getter over a string index parameter reads the string of its
//!   own call
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 6.6, 23.4
//! expect: run
//! stdout: 20900
//! notes: Pins #8941 (a string index). Each pass appends one character, so pass p (p = 1..200)
//!   indexes with a string of length p; the getter returns p + 3 and the stored value is p
//!   + 4. Hand sum over p = 1..200 of (p + 4) = 20100 + 800 = 20900.
module Conf;
import System;
public class H
  public n: int;
  public property [s: string]: int
    get => (var g := lambda(): int => 3; var q := g(); var k := lambda(): int => s.Length + q; k());
    set => n := n + value;
  end;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var i := 0;
    var s := "";
    while i < 200 do s := s + "a"; h[s] += 1; i := i + 1; end;
    Console.WriteLine(h.n);
    return 0;
  end Main;
end Program;
