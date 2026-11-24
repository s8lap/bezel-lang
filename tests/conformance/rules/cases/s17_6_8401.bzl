//! case: s17_6_8401
//! doc: spec
//! section: 17.6
//! title: a compound indexer assignment whose getter throws skips the setter and counts the
//!   throws
//! rule: The indexer's getter and setter are each called once.
//! cites: 17.6, 23.4, 27.5
//! expect: run
//! stdout: 39391 5
//! notes: Pins #9065 (leak-free capture cells in both accessors; the corpus pins the output).
//!   Getter: closure `g` returning `i`, then `q + (n mod 7)`, throws for i mod 40 = 39.
//!   Setter: closure `k` = `i + value`, `n := n + k()`. `h[i] += 1` is get, add, set. Five
//!   iterations throw (i = 39, 79, 119, 159, 199) so the setter is skipped and the catch
//!   arm counts them.
module Conf;
import System;
public class H
  public n: int;
  public c: int;
  public method Hit() c := c + 1; end Hit;
  public property [i: int]: int
    get => (var g := lambda(): int => i; var q := g(); (if i mod 40 = 39 then throw new InvalidOperationException("g") else q + (n mod 7)));
    set => do
      var k := lambda(): int => i + value;
      n := n + k();
    end;
  end;
end H;
public class Program
  public static method Main(): int
    using var h := new H();
    var i := 0;
    while i < 200 do
      try
        h[i] += 1;
      catch
        InvalidOperationException => h.Hit(),
      end try;
      i := i + 1;
    end while;
    Console.Write(h.n);
    Console.Write(" ");
    Console.Write(h.c);
    return 0;
  end Main;
end Program;
