//! case: s28_9_7416
//! doc: spec
//! section: 28.9
//! title: a closure stored by a false guard three try levels deep stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5, 31.2.1
//! expect: run
//! output: 40200
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
      try
        try
          b := g;
          if k >= 0 then
            throw new InvalidOperationException("x");
          end;
        catch
          var e: InvalidOperationException when d.TryAdd(1, b) and (k < 0) => do var z := 0; end
        end try;
      catch
        var e2: InvalidOperationException when k < 0 => do var z2 := 0; end
      end try;
    catch
      var e3: InvalidOperationException => do b := H; end
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
