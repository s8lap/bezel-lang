//! case: s28_3_4305
//! doc: spec
//! section: 28.3
//! title: an owned IEnumerator parameter moved on one path and disposed on the other is disposed once
//! rule: the parameter takes ownership of the value at the call site; the method body is responsible for discharging it before returning.
//! cites: 28.1, 28.3.1, 29.7
//! expect: run
//! output: True
//!   True
//!   fin
//!   True
//!   fin
//!   end
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

  public static method Fwd(owned e: IEnumerator<int>, c: bool)
    Console.WriteLine(e.MoveNext());
    if c then
      Y.Sink(move e);
    else
      e.Dispose();
    end if;
  end Fwd;
end Y;

public class Program
  public static method Main()
    Y.Fwd(G.Gen().GetEnumerator(), true);
    Y.Fwd(G.Gen().GetEnumerator(), false);
    Console.WriteLine("end");
  end Main;
end Program;
