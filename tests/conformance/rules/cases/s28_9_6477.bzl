//! case: s28_9_6477
//! doc: spec
//! section: 28.9
//! title: a function value bound from `??` into an Object local and added to
//!   an Object array stays callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 7.7, 8.6, 28.9
//! expect: run
//! stdout: 40200
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Fill(os: Array<Object>, k: int)
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var n: Fn? := null;
    var o: Object := n ?? g;
    os.Add(o);
  end Fill;
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var os: Array<Object> := new Array<Object>();
      Fill(os, i);
      var f := os[0] as Fn;
      total := total + f!(1, 2) + f!(4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
