//! case: s7_7_6901
//! doc: spec
//! section: 7.7
//! title: a function value chosen by ?? and passed through a record and a generic identity
//!   is called
//! rule: `a ?? b` returns `a` if non-null, else `b`.
//! cites: 7.7, 9.1, 27.7, 28.9
//! expect: run
//! stdout: 8
//! notes: The record's generated constructor stores the argument (9.1); invoking a
//!   function-typed component is an ordinary call (27.7).
module Conf;
public record RB(F: (int, int) -> int);
public class C
  public static method Id<T>(x: T): T => x;
  public static method Run(k: int): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var n: ((int, int) -> int)? := null;
    var rb := new RB(n ?? g);
    var g1 := C.Id<(int, int) -> int>(n ?? g);
    return rb.F(5, 2) + g1(5, 2);
  end Run;
  public static method Main()
    Console.WriteLine(C.Run(1));
  end Main;
end C;
