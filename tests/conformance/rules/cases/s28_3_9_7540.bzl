//! case: s28_3_9_7540
//! doc: spec
//! section: 28.3.9
//! title: moving an owned local while a closure capturing a borrow alias of it is still live is rejected
//! rule: is a compile error, unless the kill is unfrozen
//! cites: 28.3.9, 28.3
//! expect: reject
//! diagnostic: cannot consume
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
    var z := Sink(move t);
    return f();
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
