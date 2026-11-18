//! case: s28_3_8_7541
//! doc: spec
//! section: 28.3.8
//! title: a returned capture-free closure bound through a local alias chain of function values is admitted
//! rule: A capture-free function value is admitted at each of those positions
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
  method F(): () -> int
    var f := lambda() => 3; var g := f;
    return g;
  end F;
  public method Run(i: int): int var f := F(); return f(); end Run;
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
