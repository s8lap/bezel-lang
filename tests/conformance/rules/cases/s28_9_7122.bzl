//! case: s28_9_7122
//! doc: spec
//! section: 28.9
//! title: a closure local captured by a local lambda and only called, not stored, gives the call results
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 40000
module Conf;
public class C
  public static method Run(k: int): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var l: (int) -> int := lambda(z: int) => g(z, 1) + k;
    return l(3) + g(1, 2) + k;
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
