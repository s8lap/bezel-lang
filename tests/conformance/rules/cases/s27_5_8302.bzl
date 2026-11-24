//! case: s27_5_8302
//! doc: spec
//! section: 27.5
//! title: lambdas in an expression-bodied getter and a block setter read their own call's index
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 6.6, 23.4
//! expect: run
//! stdout: 40600
//! notes: Pins #8941 (the getter-then-setter shape). Per pass the getter returns i + 3, `+= 1`
//!   passes value = i + 4 to the setter, whose lambda returns i + value + 1 - 1 = 2i + 4,
//!   added to n. Hand sum over i = 0..199 of (2i + 4) = 2 * 19900 + 800 = 40600.
module Conf;
import System;
public class H
  public n: int;
  public property [i: int]: int
    get => (var g := lambda(): int => 3; var q := g(); var k := lambda(): int => i + q; k());
    set => do var g := lambda(): int => 1; var q := g(); var k := lambda(): int => i + value + q - 1; n := n + k(); end;
  end;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var i := 0;
    while i < 200 do h[i] += 1; i := i + 1; end;
    Console.WriteLine(h.n);
    return 0;
  end Main;
end Program;
