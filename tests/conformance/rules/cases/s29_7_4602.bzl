//! case: s29_7_4602
//! doc: spec
//! section: 29.7
//! title: two owned enumerator parameters are both disposed when the callee throws
//! rule: When a consumer stops iterating early — `break`, exception, or `using var` ending before exhaustion — the iterator is disposed.
//! cites: 28.3, 29.8
//! expect: run
//! output: fin
//!   fin
//!   caught
//!   fin
//!   fin
//!   caught
//!   fin
//!   fin
//!   caught
//! issue: 8436
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

public class Y
  public static method Take(owned e: IEnumerator<int>, owned f: IEnumerator<int>)
    var ok := e.MoveNext();
    var ok2 := f.MoveNext();
    throw new InvalidOperationException("boom");
  end Take;
end Y;

public class Program
  public static method Main()
    var i := 0;
    while i < 3 do
      try
        Y.Take(G.Gen().GetEnumerator(), G.Gen().GetEnumerator());
      catch
        InvalidOperationException => Console.WriteLine("caught"),
      end try;
      i := i + 1;
    end while;
  end Main;
end Program;
