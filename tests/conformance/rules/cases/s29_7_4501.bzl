//! case: s29_7_4501
//! doc: spec
//! section: 29.7
//! title: a suspended generator enumerator moved twice is disposed once, running its
//!   finally
//! rule: When a consumer stops iterating early — `break`, exception, or `using var`
//!   ending before exhaustion — the iterator is disposed.
//! cites: 28.8.1, 28.3
//! expect: run
//! output: fin
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
      var e := G.Gen().GetEnumerator();
      var ok := e.MoveNext();
      var m := move e;
      var m2 := move m;
      ok := m2.MoveNext();
      i := i + 1;
    end;
    Console.WriteLine("end");
    return 0;
  end Main;
end K;
