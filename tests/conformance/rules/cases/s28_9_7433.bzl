//! case: s28_9_7433
//! doc: spec
//! section: 28.9
//! title: a closure added to a collection stays callable after an exception unwinds past its slot
//!   reassignment
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! output: 40200
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Boom(k: int)
    if k mod 2 = 1 then
      throw new InvalidOperationException("x");
    end;
  end Boom;
  public static method Fill(fs: Array<Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
    using var sb := new StringBuilder();
    sb.Append("a");
    fs.Add(b);
    Boom(k);
    b := g;
  end Fill;
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      try
        Fill(fs, i);
      catch
        var e: InvalidOperationException => do var z := 0; end
      end try;
      total := total + fs[0](1, 2) + fs[0](4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
