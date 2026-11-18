//! case: s28_3_8_7544
//! doc: spec
//! section: 28.3.8
//! title: a closure over a borrow alias is immediately invoked and its value returned
//! rule: it may be immediately invoked
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
  method F(k: int): int
    var t := new Cell(3); var b := t;
    return (lambda() => b.v)();
  end F;
  public method Run(i: int): int return F(i); end Run;
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
