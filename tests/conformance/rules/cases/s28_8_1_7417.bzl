//! case: s28_8_1_7417
//! doc: spec
//! section: 28.8.1
//! title: a catch guard reading an owned local moved into the throwing call is rejected
//! rule: the caller's binding is moved-from on the exception edge exactly as on the normal one
//! cites: 28.8.1, 31.2.1
//! expect: reject
//! diagnostic: already moved out
module Conf;
type Fn := (int, int) -> int;
public class Box
  public N: int := 0;
  public constructor()
  end constructor;
end Box;
public class C
  private static readonly H: Fn := lambda(x: int, y: int) => x * y;
  public static method Take(owned o: Box, k: int)
    if k >= 0 then
      throw new ArgumentException("y");
    end;
  end Take;
  public static method Fill(fs: Array<Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => x - y + k;
    var b: Fn := lambda(x: int, y: int) => 0;
    var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();
    var o := new Box();
    b := H;
    try
      b := g;
      Take(move o, k);
    catch
      var e: ArgumentException when o.N = 0 and d.TryAdd(1, b) => do var z := 0; end
    end try;
    b := H;
    fs.Add(d[1]);
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      Fill(fs, i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
