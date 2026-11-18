//! case: s28_9_7102
//! doc: spec
//! section: 28.9
//! title: a closure local assigned (`b := g;`) and stored inside a loop body stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 20000
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Fill(fs: Array<Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var b: Fn := lambda(x: int, y: int) => 0;
    b := g;
    for var j in 0..<3 do
      if j = 2 and k mod 2 = 0 then
        fs.Add(b);
      end if;
    end for;
  end Fill;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      Fill(fs, i);
      if fs.Count > 0 then
        s := s + fs[0](1, 2) + fs[0](4, 1);
      end if;
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
