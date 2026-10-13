//! case: s29_8_0026
//! doc: spec
//! section: 29.8
//! title: an owned-parameter iterable from which no enumerator was produced disposes the parameter itself
//! rule: an iterable from which no enumerator was produced — its right never
//!   spent, or spent only by a transfer to a composed query or a callee that
//!   never enumerated it — disposes them itself when it is disposed.
//! cites: 29.8, 28.10
//! expect: run
//! output: bound
//!   closed
//!   after
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

  public static method Main()
    do
      using var xs := Echo(new Lines());
      Console.WriteLine("bound");
    end;
    Console.WriteLine("after");
  end Main;
end Program;
