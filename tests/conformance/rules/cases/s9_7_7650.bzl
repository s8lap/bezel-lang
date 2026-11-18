//! case: s9_7_7650
//! doc: spec
//! section: 9.7
//! title: a record constructed with a lambda that captures an owned local is rejected
//! rule: A component whose declared type is a function type admits only a capture-free function value, at construction and in `with`
//! cites: 9.7, 28.3.8
//! expect: reject
//! diagnostic: capture-bearing function value may not be stored in the record component
module Conf;
import System;
class Cell
  public static n: int := 0;
  public v: int;
  public constructor(x: int) this.v := x; Cell.n := Cell.n + 1; end constructor;
  destructor Cell.n := Cell.n - 1; end destructor;
end Cell;
record R(f: () -> int);
class H
  public method M(): R
    var t := new Cell(5);
    var r := new R(lambda() => t.v);
    return r;
  end M;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do
      var r := h.M();
      acc := acc + r.f();
    end;
    Console.WriteLine(acc);
    Console.WriteLine(Cell.n);
    return 0;
  end Main;
end Program;
