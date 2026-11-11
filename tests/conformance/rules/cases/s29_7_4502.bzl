//! case: s29_7_4502
//! doc: spec
//! section: 29.7
//! title: breaking out of a loop over a moved IEnumerable disposes the iterator and
//!   runs its finally once
//! rule: When a consumer stops iterating early — `break`, exception, or `using var`
//!   ending before exhaustion — the iterator is disposed.
//! cites: 28.8.1
//! expect: run
//! output: x
//!   fin
//!   x
//!   fin
//!   end
//! issue: 7960
module Conf;

public class G
  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
      Console.WriteLine("fin");
    end;
  end Gen;
end G;

public class K
  public static method Main(): int
    var i := 0;
    while i < 2 do
      var s: IEnumerable<int> := G.Gen();
      var t := move s;
      for var x in t do Console.WriteLine("x"); break; end;
      i := i + 1;
    end;
    Console.WriteLine("end");
    return 0;
  end Main;
end K;
