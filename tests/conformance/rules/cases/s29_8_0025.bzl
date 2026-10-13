//! case: s29_8_0025
//! doc: spec
//! section: 29.8
//! title: a second enumeration across an IEnumerable parameter boundary raises InvalidOperationException
//! rule: a second consumption is a compile error where the flow analysis can
//!   see it and raises `InvalidOperationException` where it is reachable only
//!   at run time ... The first `GetEnumerator()` (or `GetAsyncEnumerator()`)
//!   transfers the frame's `owned` and move-only parameters to the enumerator
//!   it produces; that enumerator disposes any such parameter the body has not
//!   moved out of when it completes or is disposed
//! cites: 29.8, 29.11
//! expect: run
//! output: pass 1
//!   closed
//!   spent
//! issue: 5824
module Conf;

public class Lines
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Lines;

public class Program
  public static method Echo(owned source: Lines): owned IEnumerable<int>
    yield return 1;
  end Echo;

  public static method Twice(xs: IEnumerable<int>)
    for var a in xs do
      Console.WriteLine($"pass {a}");
    end;
    try
      for var b in xs do
        Console.WriteLine($"again {b}");
      end;
    catch
      InvalidOperationException => Console.WriteLine("spent")
    end try;
  end Twice;

  public static method Main()
    using var xs := Echo(new Lines());
    Twice(xs);
  end Main;
end Program;
