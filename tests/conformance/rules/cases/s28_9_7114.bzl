//! case: s28_9_7114
//! doc: spec
//! section: 28.9
//! title: a closure local captured by a lambda over a local cell, both only called, gives the call results
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 40400
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Run(k: int): int
    var m := k + 1;
    var g: Fn := lambda(x: int, y: int) => x - y + m;
    var l: (int) -> int := lambda(z: int) => g(z, 1);
    return l(3) + g(1, 2);
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
