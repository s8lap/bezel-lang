//! case: s18_5_6901
//! doc: spec
//! section: 18.5
//! title: a function value kept by a call inside an interpolation hole stays callable after
//!   the scope
//! rule: A `$` prefix introduces interpolation. Any expression may appear inside `{}`:
//! cites: 18.5, 28.9
//! expect: run
//! stdout: 20500
//! notes: A function value is runtime-managed (28.9), so a closure kept from inside a hole
//!   has unbounded lifetime.
module Conf;
public record RB(F: (int, int) -> int);
public class C
  public static method KeepR(r: RB, fs: Array<(int, int) -> int>): int
    fs.Add(r.F);
    return 1;
  end KeepR;
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var rb := new RB(g);
    var s := $"{C.KeepR(rb, fs)}";
    if s.Length = 0 then Console.WriteLine(s); end;
  end Fill;
  public static method Main()
    var t := 0;
    for var i in 0..<200 do
      var fs := new Array<(int, int) -> int>();
      C.Fill(fs, i);
      for var f in fs do t := t + f(5, 2); end;
    end;
    Console.WriteLine(t);
  end Main;
end C;
