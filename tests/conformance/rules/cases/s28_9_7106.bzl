//! case: s28_9_7106
//! doc: spec
//! section: 28.9
//! title: in an async body, a closure local captured by a local lambda is stored on half the iterations
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5, 30.5
//! expect: run
//! stdout: 40300
module Conf;
type Fn := (int, int) -> int;
public class C
  public static async method Fill(fs: Array<Fn>, k: int): Task<int>
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var l: (int) -> int := lambda(z: int) => g(z, 1);
    var t := l(3);
    if k mod 2 = 0 then
      fs.Add(g);
    end if;
    return t;
  end Fill;
  public static async method Main(): Task<int>
    var s := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      s := s + await Fill(fs, i);
      if fs.Count > 0 then
        s := s + fs[0](1, 2) + fs[0](4, 1);
      end if;
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
