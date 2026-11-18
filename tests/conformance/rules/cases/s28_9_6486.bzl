//! case: s28_9_6486
//! doc: spec
//! section: 28.9
//! title: an alias added to an array that outlives the block both names are
//!   declared in stays callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 400
module Conf;
public class C
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      do
        var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
        var a: (int, int) -> int := g;
        fs.Add(a);
      end;
      total := total + fs[0](1, 2) + fs[0](4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
