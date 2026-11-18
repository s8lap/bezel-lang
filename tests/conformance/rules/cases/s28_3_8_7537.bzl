//! case: s28_3_8_7537
//! doc: spec
//! section: 28.3.8
//! title: a closure over a borrow alias written through an out parameter is rejected
//! rule: a function value may not be stored in a field, in any collection, in a property or indexer, or
//!   through an `out` or `ref` parameter
//! cites: 28.3.8
//! expect: reject
//! diagnostic: out/ref parameter
module Conf;
import System;
import System.Collections.Generic;
type Fn := () -> int;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
record R(A: int);
record RefRec(W: Cell?, N: int);
class H
  static method Mk(out o: Fn): int
    var t := new Cell(3); var b := t;
    o := lambda() => b.v;
    return 0;
  end Mk;
  public method Run(i: int): int
    var o: Fn := lambda() => 0;
    var z := Mk(out o);
    return o();
  end Run;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do acc := acc + h.Run(i); end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
