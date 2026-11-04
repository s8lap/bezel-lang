//! case: s30_10_0013
//! doc: spec
//! section: 30.10
//! title: the borrowing WhenAll overload delivers results in input order, not completion order
//! rule: `Task.WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>` ... If
//!   every input succeeded it completes successfully (for the generic form,
//!   with the results in input order).
//! cites: 30.10
//! expect: run
//! output: 30
//!   10
//!   20
module Conf;

public class Work
  public static async method MakeAsync(n: int): Task<int>
    await Task.Delay(n);
    return n * 10;
  end MakeAsync;
end Work;

public class Program
  public static async method Main(): Task
    var a := Work.MakeAsync(3);
    var b := Work.MakeAsync(1);
    var c := Work.MakeAsync(2);
    var ts: Array<Task<int>> := [a, b, c];
    using var all := Task.WhenAll(ts);
    var results := await all;
    for var r in results do
      Console.WriteLine(r);
    end for;
  end Main;
end Program;
