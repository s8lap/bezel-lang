//! case: s28_3_4_7615
//! doc: spec
//! section: 28.3.4
//! title: a closure over an int copied from an alias of a plain parameter may be returned
//! rule: Parameter `p` (no modifier) | Borrowed (source: caller)
//! cites: 28.3.4, 28.3.8
//! expect: run
//! stdout: 1600
module Conf;
import System;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
class H
  public static method G(w: Cell): () -> int
    var b := w;
    var n := b.v;
    return lambda() => n;
  end G;
end H;
public class Program
  public static method Main(): int
    var total := 0;
    for var i in 0 ..< 200 do
      var f: () -> int := lambda() => 0;
      do
        var c := new Cell(8);
        f := H.G(c);
      end;
      total := total + f();
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
