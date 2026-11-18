//! case: s28_3_8_7542
//! doc: spec
//! section: 28.3.8
//! title: a closure assigned to a local of a nested block that does not enclose the source scope is admitted
//! rule: whose scope does not strictly enclose any source's scope
//! cites: 28.3.8
//! expect: run
//! stdout: 9
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
  public method Run(i: int): int
    var s := 0;
    do
      var c := new Cell(3);
      do
        var g: () -> int := lambda() => 0;
        var f := lambda() => c.v;
        g := f;
        s := g();
      end;
    end;
    return s;
  end Run;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<3 do acc := acc + h.Run(i); end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
