//! case: s28_3_0296
//! doc: spec
//! section: 28.3
//! title: named-argument calls on a new StringBuilder temporary that throw mid-arguments unwind cleanly
//! rule: Unbound owned temporaries are disposed at their full expression's end, after their uses, in reverse production order — on the normal edge as on the exception edge, where the pass runs during unwinding.
//! cites: 22.5, 28.3
//! expect: run
//! output: 5050
module Conf;
import System.Text;
class C
  public static method Ts(s: string): string return s; end Ts;
  public static method Boom(i: int): int
    if i mod 2 = 0 then throw new InvalidOperationException("b"); end;
    return 1;
  end Boom;
  public static method Main(): int
    var tot := 0;
    for var i in 0..<100 do
      try
        new StringBuilder("abc").Insert(value := Ts("Z"), index := Boom(i));
        new StringBuilder("abc").Remove(length := Boom(i), startIndex := 0).Append("x");
        tot := tot + 1;
      catch
        InvalidOperationException => tot := tot + 100;
      end try;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end C;
