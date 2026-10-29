//! case: s28_3_0289
//! doc: spec
//! section: 28.3
//! title: an alias and its owner read after an await in an async Main are live
//! rule: An unmarked read of a class-typed binding never consumes: `var b := a;` is always a borrow for class-typed `a`.
//! cites: 28.2, 28.9, 28.11
//! expect: run
//! output: 606
module Conf;
class Program
  public static async method Main(): Task<int>
    var sum := 0;
    for var i in 0..100 do
      var a: Array<int> := [1,2,3];
      var b := a;
      await Task.Delay(0);
      sum := sum + b.Count + a.Count;
    end;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
