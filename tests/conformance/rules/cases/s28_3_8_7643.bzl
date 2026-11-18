//! case: s28_3_8_7643
//! doc: spec
//! section: 28.3.8
//! title: a closure over a plain OwningArray parameter may not be stored in a field
//! rule: may not be stored in a field
//! cites: 28.3.4, 28.3.8
//! expect: reject
//! diagnostic: closure stored in a field
module Conf;
import System;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
class H
  private _fn: () -> int;
  public constructor() this._fn := lambda() => 0; end constructor;
  public method S(w: OwningArray<Cell>): int
    this._fn := lambda() => w[0].v;
    return 0;
  end S;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    Console.WriteLine(1);
    return 0;
  end Main;
end Program;
