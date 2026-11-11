//! case: s28_3_4307
//! doc: spec
//! section: 28.3
//! title: an owned IEnumerator parameter moved to a callee and followed by a throw is disposed once
//! rule: the parameter takes ownership of the value at the call site; the method body is responsible for discharging it before returning.
//! cites: 28.1, 28.3.1, 31.2
//! expect: run
//! output: True
//!   fin
//!   caught
//! issue: 7961
module Conf;

class G
  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
      Console.WriteLine("fin");
    end try;
  end Gen;
end G;

class Y
  public static method Sink(owned e: IEnumerator<int>)
    Console.WriteLine(e.MoveNext());
  end Sink;

  public static method Fwd(owned e: IEnumerator<int>)
    Y.Sink(move e);
    throw new InvalidOperationException("boom");
  end Fwd;
end Y;

public class Program
  public static method Main()
    try
      Y.Fwd(G.Gen().GetEnumerator());
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught"),
    end try;
  end Main;
end Program;
