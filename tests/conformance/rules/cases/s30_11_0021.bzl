//! case: s30_11_0021
//! doc: spec
//! section: 30.11
//! title: an in-scope read var t := tcs.Task! and t.Result is admitted
//! rule: the borrow is bounded by `tcs`'s scope
//! cites: 30.11, 17.8, 28.3
//! expect: run
//! stdout: 8
//! issue: 8671
module Conf;

public class Program
  public static method Get(): int
    using var tcs := new TaskCompletionSource<int>();
    tcs.SetResult(8);
    var t := tcs.Task!;
    return t.Result;
  end Get;

  public static method Main()
    Console.WriteLine(Get());
  end Main;
end Program;
