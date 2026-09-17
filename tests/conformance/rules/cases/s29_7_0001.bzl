//! case: s29_7_0001
//! doc: spec
//! section: 29.7
//! title: breaking out of a for loop early disposes the iterator's pending using var
//! rule: When a consumer stops iterating early — `break`, exception, or
//!   `using var` ending before exhaustion — the iterator is disposed. The
//!   state machine runs the pending cleanup actions active at the point of
//!   suspension.
//! cites: 29.7
//! expect: run
//! output: cleanup
//!   after
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("cleanup");
  end destructor;
end Resource;

public class Program
  public static method Gen(): IEnumerable<int>
    using var r := new Resource();
    yield return 1;
    yield return 2;
  end Gen;

  public static method Main()
    for var x in Gen() do
      break;
    end;
    Console.WriteLine("after");
  end Main;
end Program;
