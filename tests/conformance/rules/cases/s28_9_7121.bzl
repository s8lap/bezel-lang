//! case: s28_9_7121
//! doc: spec
//! section: 28.9
//! title: a closure local assigned into another closure local that is only called gives the call result
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 19700
module Conf;
public class C
  public static method Run(k: int): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var b: (int, int) -> int := lambda(x: int, y: int) => 0;
    b := g;
    return b(1, 2);
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      s := s + Run(i);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
