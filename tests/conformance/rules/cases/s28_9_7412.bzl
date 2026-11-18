//! case: s28_9_7412
//! doc: spec
//! section: 28.9
//! title: a closure stored from a catch guard stays callable after its slot is reassigned
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5, 31.2.1
//! expect: run
//! output: 41400
module Conf;
type Fn := (int, int) -> int;
public class C
  private static readonly H: Fn := lambda(x: int, y: int) => x * y;
  public static method Fill(fs: Array<Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var b: Fn := lambda(x: int, y: int) => 0;
    var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();
    b := H;
    try
      b := lambda(x: int, y: int) => x + y + k;
      if k >= 0 then
        throw new InvalidOperationException("x");
      end;
    catch
      var e: InvalidOperationException when d.TryAdd(1, b) => do var z := 0; end
    end try;
    b := H;
    fs.Add(d[1]);
  end Fill;
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      Fill(fs, i);
      total := total + fs[0](1, 2) + fs[0](4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
