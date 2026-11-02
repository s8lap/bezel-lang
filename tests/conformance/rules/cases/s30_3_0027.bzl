//! case: s30_3_0027
//! doc: spec
//! section: 30.3
//! title: Wait and a null-conditional Wait over a completed task leave Result readable
//! rule: they block the calling thread until the task completes
//! cites: 30.3, 7.5
//! expect: run
//! output: 4950
module Conf;

public class Program
  public static method Main(): int
    var n := 0;
    var i := 0;
    while i < 100 do
      using var t := Task.FromResult(i);
      t.Wait();
      var u: Task<int>? := t;
      u?.Wait();
      if t.IsCompleted then
        n += t.Result;
      end;
      i += 1;
    end;
    Console.WriteLine(n.ToString());
    return 0;
  end Main;
end Program;
