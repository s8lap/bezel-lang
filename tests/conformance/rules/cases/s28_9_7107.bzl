//! case: s28_9_7107
//! doc: spec
//! section: 28.9
//! title: a closure local assigned (`b := h;`) where h captures a string local stays callable once stored
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 40200
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Fill(fs: Array<Fn>, k: int)
    var b: Fn := lambda(x: int, y: int) => 0;
    var str := "ab" + k.ToString();
    var h: Fn := lambda(x: int, y: int) => x - y + k + str.Length - str.Length;
    b := h;
    fs.Add(b);
  end Fill;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      Fill(fs, i);
      s := s + fs[0](1, 2) + fs[0](4, 1);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
