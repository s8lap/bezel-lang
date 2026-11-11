//! case: s29_7_4601
//! doc: spec
//! section: 29.7
//! title: an enumerator owned by a parameter is disposed, running its finally, when the callee throws
//! rule: When a consumer stops iterating early — `break`, exception, or `using var` ending before exhaustion — the iterator is disposed.
//! cites: 28.3, 29.8
//! expect: run
//! output: thrown
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
  public static method Take(owned e: IEnumerator<int>)
    var ok := e.MoveNext();
    Console.WriteLine("thrown");
    throw new InvalidOperationException("boom");
  end Take;
end Y;

public class Program
  public static method Main()
    try
      Y.Take(G.Gen().GetEnumerator());
    catch
      InvalidOperationException => Console.WriteLine("caught"),
    end try;
  end Main;
end Program;
