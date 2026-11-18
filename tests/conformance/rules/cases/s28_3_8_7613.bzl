//! case: s28_3_8_7613
//! doc: spec
//! section: 28.3.8
//! title: a closure over a two-hop alias of a plain parameter may not be returned
//! rule: may not be returned from the enclosing method
//! cites: 28.3.4, 28.3.8
//! expect: reject
//! diagnostic: returned
module Conf;
import System;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
class H
  public static method G(w: Cell): () -> int
    var b := w;
    var c := b;
    return lambda() => c.v;
  end G;
end H;
public class Program
  public static method Main(): int
    var f: () -> int := lambda() => 0;
    do
      var c := new Cell(8);
      f := H.G(c);
    end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
