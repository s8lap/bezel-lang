//! case: s28_3_8_7543
//! doc: spec
//! section: 28.3.8
//! title: a closure and its local alias are passed to a plain function-typed parameter that only invokes it
//! rule: call-bounded at a plain parameter
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
  static method Apply(fn: () -> int): int return fn(); end Apply;
  public method Run(i: int): int
    var t := new Cell(3); var b := t;
    var f := lambda() => b.v;
    var g := f;
    return Apply(f) + Apply(g) - 3;
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
