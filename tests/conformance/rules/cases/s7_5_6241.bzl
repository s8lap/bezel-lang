//! case: s7_5_6241
//! doc: spec
//! section: 7.5
//! title: ?.Sort and ?.Sorted with an un-annotated comparer lambda on a null
//!   receiver yield null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the
//!   access.
//! cites: 7.5, 4.4
//! expect: run
//! stdout: null ok
module Conf;

record W(K: string, V: int);

public class Program
  public static method Main()
    var n: Array<W>? := null;
    n?.Sort(lambda(x, y) => x.K.CompareTo(y.K));
    var s := n?.Sorted(lambda(x, y) => x.K.CompareTo(y.K));
    if s = null then
      Console.WriteLine("null ok");
    end;
  end Main;
end Program;
