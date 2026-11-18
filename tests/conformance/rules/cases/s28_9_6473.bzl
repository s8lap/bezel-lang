//! case: s28_9_6473
//! doc: spec
//! section: 28.9
//! title: a function value added through `??` inside a lambda body loop stays
//!   callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 7.7, 27.5, 28.9
//! expect: run
//! stdout: 40200
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Fill(fs: Array<Fn>, k: int)
    var body := lambda() => do
      var g: Fn := lambda(x: int, y: int) => x - y + k;
      var n: Fn? := null;
      for var j in 0..<2 do
        fs.Add(n ?? g);
      end for;
    end;
    body();
  end Fill;
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      Fill(fs, i);
      total := total + fs[0](1, 2) + fs[1](4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
