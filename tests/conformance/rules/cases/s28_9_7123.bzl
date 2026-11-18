//! case: s28_9_7123
//! doc: spec
//! section: 28.9
//! title: in a generic method body, a closure local assigned (`b := g;`) into a kept store stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 40200
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Fill<T>(fs: Array<Fn>, k: int, tag: T)
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var b: Fn := lambda(x: int, y: int) => 0;
    b := g;
    fs.Add(b);
  end Fill;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      Fill<int>(fs, i, 7);
      t := t + fs[0](1, 2) + fs[0](4, 1);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
