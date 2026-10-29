//! case: s28_3_0300
//! doc: spec
//! section: 28.3
//! title: chained Append and AppendLine calls on builder temporaries return the documented lengths
//! rule: Unbound owned temporaries are disposed at their full expression's end, after their uses, in reverse production order — on the normal edge as on the exception edge, where the pass runs during unwinding.
//! cites: 22.5, 28.3
//! expect: run
//! output: 990
module Conf;
import System.Text;
class C
  public static method Main(): int
    var tot := 0;
    var other := new StringBuilder("x");
    for var i in 0..<100 do
      tot := tot + new StringBuilder("a").Append(i).Append('c').Append(true).Append(other).ToString().Length;
      tot := tot + new StringBuilder("a").AppendLine().AppendLine("z").Clear().Append("q").ToString().Length;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end C;
