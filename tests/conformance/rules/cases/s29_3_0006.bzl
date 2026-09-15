//! case: s29_3_0006
//! doc: spec
//! section: 29.3
//! title: an IEnumerable<T>-returning iterator method reruns its body on each GetEnumerator call
//! rule: An iterator method's return type is `IEnumerable<T>` (re-iterable:
//!   each call to `GetEnumerator()` runs the body again from the start).
//! cites: 29.3
//! expect: run
//! stdout: 2
module Conf;

public class Program
  private _calls: int := 0;

  public method Gen(): IEnumerable<int>
    _calls := _calls + 1;
    yield return 1;
  end Gen;

  public static method Main()
    var p := new Program();
    var e := p.Gen();
    using var it1 := e.GetEnumerator();
    _ := it1.MoveNext();
    using var it2 := e.GetEnumerator();
    _ := it2.MoveNext();
    Console.WriteLine(p._calls);
  end Main;
end Program;
