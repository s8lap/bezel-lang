//! case: s28_9_7111
//! doc: spec
//! section: 28.9
//! title: a closure local stored in a collection while the lambda capturing it is returned stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 60500
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Fill(fs: Array<Fn>, k: int): (int) -> int
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var l: (int) -> int := lambda(z: int) => g(z, 1);
    fs.Add(g);
    return l;
  end Fill;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      var l := Fill(fs, i);
      s := s + fs[0](1, 2) + fs[0](4, 1) + l(3);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
