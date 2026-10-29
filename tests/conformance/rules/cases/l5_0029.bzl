//! case: l5_0029
//! doc: stdlib
//! section: 5
//! title: ToArray on an IList receiver returns a fresh copy independent of the source
//! rule: ToArray() returns a fresh Array<T> containing the materialized sequence (stdlib 5); ToArray is not declared by IList<T>, so the extension is reached, and writing the copy does not write the source.
//! cites: 5, 4.1
//! expect: run
//! output: 5
//!   104
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    var t := l.ToArray();
    t[0] := 100;
    Console.WriteLine(a[0]);
    Console.WriteLine(t[0] + t.Count);
  end Main;
end Program;
