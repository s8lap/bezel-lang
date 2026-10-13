//! case: s28_3_0155
//! doc: spec
//! section: 28.3
//! title: a lazy query enumerated through an unbound call before the transfer is admitted
//! rule: The remedies: transfer first, then re-derive the frame from the new owner;
//!   enumerate the frame through an unbound call (`for var x in Each(xs.AsSpan()) do … end;`,
//!   whose hidden `__source` is disposed at the loop's exit) or dispose it
//!   (`it.Dispose();`) before the transfer
//! cites: 28.3, 29.2
//! expect: run
//! stdout: 8
//! issue: 5698
module Conf;


public class Program
  public static method Take(owned a: Array<int>): int => a.Count;

  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    var n := 0;
    for var x in xs.Where(lambda(v) => v > 1) do
      n := n + x;
    end;
    n := n + Take(move xs);
    Console.WriteLine(n.ToString());
  end Main;
end Program;
