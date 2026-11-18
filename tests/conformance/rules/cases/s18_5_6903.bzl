//! case: s18_5_6903
//! doc: spec
//! section: 18.5
//! title: a function value called only inside an interpolation hole and in the return value
//!   is evaluated
//! rule: A `$` prefix introduces interpolation. Any expression may appear inside `{}`:
//! cites: 18.5, 28.9
//! expect: run
//! stdout: 41000
module Conf;
public record RB(F: (int, int) -> int);
public class C
  public static method Sum(k: int): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var n: ((int, int) -> int)? := null;
    var rb := new RB(n ?? g);
    var s := $"{g(5, 2)}|{rb.F(5, 2)}";
    if s.Length = 0 then return 0; end;
    return g(5, 2) + rb.F(5, 2);
  end Sum;
  public static method Main()
    var t := 0;
    for var i in 0..<200 do
      t := t + C.Sum(i);
    end;
    Console.WriteLine(t);
  end Main;
end C;
