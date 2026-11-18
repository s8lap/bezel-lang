//! case: s28_9_7113
//! doc: spec
//! section: 28.9
//! title: a closure local assigned (`b := g;`), copied (`var t := b;`) and only called gives the call result
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 19900
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Run(k: int): int
    var m := k + 1;
    var g: Fn := lambda(x: int, y: int) => x - y + m;
    var b: Fn := lambda(x: int, y: int) => 0;
    b := g;
    var t := b;
    return t(1, 2);
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
