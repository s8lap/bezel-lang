//! case: s28_9_7103
//! doc: spec
//! section: 28.9
//! title: a method reference assigned (`b := g;`) into a kept collection store stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.6
//! expect: run
//! stdout: 20300
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Sub(x: int, y: int): int
    return x - y;
  end Sub;
  public static method Fill(fs: Array<Fn>, k: int)
    var g: Fn := C.Sub;
    var b: Fn := lambda(x: int, y: int) => 0;
    b := g;
    fs.Add(b);
  end Fill;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      Fill(fs, i);
      s := s + fs[0](1, 2) + fs[0](4, 1) + i;
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
