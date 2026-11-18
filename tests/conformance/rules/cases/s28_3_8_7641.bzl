//! case: s28_3_8_7641
//! doc: spec
//! section: 28.3.8
//! title: a closure over a plain OwningArray parameter may not be returned
//! rule: may not be returned from the enclosing method
//! cites: 28.3.4, 28.3.8
//! expect: reject
//! diagnostic: returned closure captures
module Conf;
import System;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
class H
  public static method G(w: OwningArray<Cell>): () -> int
    return lambda() => w[0].v;
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
