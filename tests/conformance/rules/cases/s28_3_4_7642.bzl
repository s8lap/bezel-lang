//! case: s28_3_4_7642
//! doc: spec
//! section: 28.3.4
//! title: a closure over an int copied out of a plain OwningArray parameter may be returned
//! rule: may not be returned from the enclosing method
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
  public static method G(w: OwningArray<Cell>): () -> int
    var x := w[0].v;
    return lambda() => x;
  end G;
end H;
public class Program
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var f: () -> int := lambda() => 0;
      do
        using var a := new OwningArray<Cell>();
        a.Add(new Cell(8));
        f := H.G(a);
      end;
      total := total + f();
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
