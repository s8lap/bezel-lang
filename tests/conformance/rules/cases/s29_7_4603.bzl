//! case: s29_7_4603
//! doc: spec
//! section: 29.7
//! title: an owned enumerator parameter of an instance method is disposed when the method throws
//! rule: When a consumer stops iterating early — `break`, exception, or `using var` ending before exhaustion — the iterator is disposed.
//! cites: 28.3, 29.8
//! expect: run
//! output: fin
//!   caught
//!   fin
//!   caught
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
  public method Take(owned e: IEnumerator<int>)
    var ok := e.MoveNext();
    throw new InvalidOperationException("boom");
  end Take;
end Y;

public class Program
  public static method Main()
    var i := 0;
    while i < 3 do
      try
        var y := new Y();
        y.Take(G.Gen().GetEnumerator());
      catch
        InvalidOperationException => Console.WriteLine("caught"),
      end try;
      i := i + 1;
    end while;
  end Main;
end Program;
