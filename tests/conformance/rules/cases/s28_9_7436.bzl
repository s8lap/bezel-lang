//! case: s28_9_7436
//! doc: spec
//! section: 28.9
//! title: a closure stored by a catch guard that evaluates false stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 31.2.1
//! expect: run
//! output: 20200
//!   100
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Boom(k: int)
    if k mod 2 = 1 then
      throw new InvalidOperationException("x");
    end;
  end Boom;
  public static method Fill(d: Dictionary<int, Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
    try
      Boom(k);
    catch
      var e: InvalidOperationException when d.TryAdd(0, b) and k < 0 => Console.WriteLine(0);
    end try;
    b := g;
  end Fill;
  public static method Main(): int
    var n := 0;
    var total := 0;
    for var i in 0..<200 do
      var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();
      try
        Fill(d, i);
      catch
        var e: InvalidOperationException => do n := n + 1; end
      end try;
      if d.ContainsKey(0) then
        total := total + d[0](1, 2) + d[0](4, 1);
      end;
    end for;
    Console.WriteLine(total);
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
