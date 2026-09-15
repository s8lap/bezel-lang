//! case: s29_7_0004
//! doc: spec
//! section: 29.7
//! title: MoveNext() after the enumerator is disposed returns false
//! rule: Transitions to a completed state; subsequent `MoveNext()` returns
//!   `false`.
//! cites: 29.7
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Gen(): IEnumerable<int>
    yield return 1;
  end Gen;

  public static method Main()
    var e := Gen().GetEnumerator();
    e.Dispose();
    Console.WriteLine(e.MoveNext());
  end Main;
end Program;
