//! case: l5_0030
//! doc: stdlib
//! section: 5
//! title: ToArray on an IReadOnlyList receiver returns a fresh copy of the elements
//! rule: ToArray() returns a fresh Array<T> containing the materialized sequence (stdlib 5), reachable on IReadOnlyList<T> receivers.
//! cites: 5, 4.1
//! expect: run
//! output: 3
//!   4
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    var t := r.ToArray();
    Console.WriteLine(t[1]);
    Console.WriteLine(t.Count);
  end Main;
end Program;
