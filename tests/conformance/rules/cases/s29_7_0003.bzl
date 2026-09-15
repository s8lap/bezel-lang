//! case: s29_7_0003
//! doc: spec
//! section: 29.7
//! title: a second Dispose() on an already-completed enumerator is a no-op
//! rule: Transitions to a completed state; subsequent `MoveNext()` returns
//!   `false`; second `Dispose()` is a no-op.
//! cites: 29.7
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static method Gen(): IEnumerable<int>
    yield return 1;
  end Gen;

  public static method Main()
    var e := Gen().GetEnumerator();
    e.Dispose();
    e.Dispose();
    Console.WriteLine("done");
  end Main;
end Program;
