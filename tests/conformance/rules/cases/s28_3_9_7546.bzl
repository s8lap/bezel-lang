//! case: s28_3_9_7546
//! doc: spec
//! section: 28.3.9
//! title: moving an owned local after the closure over its alias is dead is admitted
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use of it
//! cites: 28.3.9, 28.3
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
  static method Sink(owned c: Cell): int var k := c.v; return k; end Sink;
  public method Run(i: int): int
    var t := new Cell(3); var b := t;
    var f := lambda() => b.v;
    var n := f();
    var z := Sink(move t);
    return n;
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
