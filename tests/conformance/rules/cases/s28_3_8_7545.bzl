//! case: s28_3_8_7545
//! doc: spec
//! section: 28.3.8
//! title: a lambda returning a closure over a local it itself captured is admitted
//! rule: a lambda's body can return a closure only over what the lambda itself captures
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
class H
  public method Run(i: int): int
    var t := new Cell(3);
    var mk := lambda(): Fn => do var b := t; return lambda() => b.v; end;
    var g := mk();
    return g();
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
