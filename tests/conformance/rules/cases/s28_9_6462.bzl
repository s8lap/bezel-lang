//! case: s28_9_6462
//! doc: spec
//! section: 28.9
//! title: a function value reached through `??` then `as` and added to the
//!   caller's array stays callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 7.7, 17.4, 28.9
//! expect: run
//! stdout: 40200
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Fill(fs: Array<Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var n: Fn? := null;
    fs.Add(((n ?? g) as Fn)!);
  end Fill;
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      Fill(fs, i);
      total := total + fs[0](1, 2) + fs[0](4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
