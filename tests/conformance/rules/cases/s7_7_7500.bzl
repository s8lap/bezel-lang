//! case: s7_7_7500
//! doc: spec
//! section: 7.7
//! title: a function value chosen by ?? stored in a record component and passed through Id stays callable
//! rule: `a ?? b` returns `a` if non-null, else `b`
//! cites: 7.7, 9.1
//! expect: run
//! stdout: 8
module Conf;
import System;
public record RB(F: (int, int) -> int);
public class C
  public static method Id<T>(x: T): T => x;
  public static method Main(): int
    var k := 1;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var n: ((int, int) -> int)? := null;
    var rb := new RB(n ?? g);
    var g1 := C.Id<(int, int) -> int>(n ?? g);
    Console.WriteLine(rb.F(5, 2) + g1(5, 2));
    return 0;
  end Main;
end C;
