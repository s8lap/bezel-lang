//! case: s7_5_0026
//! doc: spec
//! section: 7.5
//! title: ?. over a Task<int>? reads IsCompleted and Result when non-null and short-circuits when null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//!   The result type is always nullable
//! cites: 7.5, 30.3
//! expect: run
//! issue: 7896
//! output: local-p True|7|False
//!   local-n False|-1|True
//!   call-p True|7
//!   call-n False|-1
//!   107
module Conf;

public class Program
  static method GetT(on: bool): Task<int>?
    if on then
      return Task.FromResult(7);
    end if;
    return null;
  end GetT;

  static method Show(tag: string, t: Task<int>?)
    var done := t?.IsCompleted;
    var faulted := t?.IsFaulted;
    var r := t?.Result;
    Console.WriteLine($"{tag} {done ?? false}|{r ?? -1}|{faulted = null}");
  end Show;

  public static method Main()
    using var t0 := Task.FromResult(7);
    var tn: Task<int>? := null;
    var tp: Task<int>? := t0;
    Show("local-p", tp);
    Show("local-n", tn);
    Console.WriteLine($"call-p {GetT(true)?.IsCompleted ?? false}|{GetT(true)?.Result ?? -1}");
    Console.WriteLine($"call-n {GetT(false)?.IsCompleted ?? false}|{GetT(false)?.Result ?? -1}");
    var f := lambda() => (tp?.Result ?? 0) + (tn?.Result ?? 100);
    Console.WriteLine(f());
  end Main;
end Program;
