//! case: s28_3_0298
//! doc: spec
//! section: 28.3
//! title: awaits inside named arguments of a builder temporary call evaluate in order and unwind on throw
//! rule: Unbound owned temporaries are disposed at their full expression's end, after their uses, in reverse production order — on the normal edge as on the exception edge, where the pass runs during unwinding.
//! cites: 22.5, 28.3
//! expect: run
//! output: 5295
module Conf;
import System.Text;
class C
  public static async method G(i: int): Task<string>
    await Task.Delay(0);
    return "g" + i.ToString();
  end G;
  public static async method Boom(i: int): Task<int>
    await Task.Delay(0);
    if i mod 2 = 0 then throw new InvalidOperationException("b"); end;
    return 1;
  end Boom;
  public static async method Run(): Task<int>
    var tot := 0;
    for var i in 0..<100 do
      try
        tot := tot + new StringBuilder("abc").Insert(value := await G(i), index := await Boom(i)).Length;
      catch
        InvalidOperationException => tot := tot + 100;
      end try;
    end;
    return tot;
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().Result);
    return 0;
  end Main;
end C;
