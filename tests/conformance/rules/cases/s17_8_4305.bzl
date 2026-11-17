//! case: s17_8_4305
//! doc: spec
//! section: 17.8
//! title: returning tcs?.Task! out of the scope of a nullable source local is rejected
//! rule: if `x` is borrowed, `x!` is borrowed with the same source
//! cites: 17.8, 30.11, 28.3
//! expect: reject
//! diagnostic: borrow
//! issue: 8671
module Conf;

public class Program
  public static method Get(): Task<int>
    var tcs: TaskCompletionSource<int>? := new TaskCompletionSource<int>();
    tcs!.SetResult(1);
    return tcs?.Task!;
  end Get;

  public static method Main()
    using var t := Get();
    Console.WriteLine(t.Result);
  end Main;
end Program;
