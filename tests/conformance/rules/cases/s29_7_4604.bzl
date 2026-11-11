//! case: s29_7_4604
//! doc: spec
//! section: 29.7
//! title: an owned enumerator already exhausted is not disposed again when the callee throws
//! rule: Transitions to a **completed** state: the machine's own bookkeeping records exhaustion, so nothing resumes and no suspended body runs again.
//! cites: 28.3, 29.8
//! expect: run
//! output: fin
//!   caught
//!   fin
//!   caught
//!   fin
//!   caught
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
    while e.MoveNext() do end while;
    throw new InvalidOperationException("boom");
  end Take;
end Y;

public class Program
  public static method Main()
    var i := 0;
    while i < 3 do
      try
        Y.Take(G.Gen().GetEnumerator());
      catch
        InvalidOperationException => Console.WriteLine("caught"),
      end try;
      i := i + 1;
    end while;
  end Main;
end Program;
