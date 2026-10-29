//! case: s28_3_0297
//! doc: spec
//! section: 28.3
//! title: a throwing call inside a short-circuited condition over a builder temporary unwinds cleanly
//! rule: Unbound owned temporaries are disposed at their full expression's end, after their uses, in reverse production order — on the normal edge as on the exception edge, where the pass runs during unwinding.
//! cites: 22.5, 28.3
//! expect: run
//! output: 2550
module Conf;
import System.Text;
class C
  public static method Boom(i: int): string
    if i mod 4 = 1 then throw new InvalidOperationException("b"); end;
    return "k";
  end Boom;
  public static method Ix(i: int): int
    if i mod 4 = 1 then throw new InvalidOperationException("c"); end;
    return 0;
  end Ix;
  public static method Main(): int
    var tot := 0;
    for var i in 0..<100 do
      try
        if i mod 2 = 1 and new StringBuilder("ab").Append(Boom(i)).Length > 0 then tot := tot + 1; end;
        if i mod 2 = 1 and new StringBuilder("ab").Insert(value := "q", index := Ix(i)).Length > 0 then tot := tot + 1; end;
      catch
        InvalidOperationException => tot := tot + 100;
      end try;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end C;
