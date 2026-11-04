//! case: s7_5_0027
//! doc: spec
//! section: 7.5
//! title: ?. over a non-generic Task? and a Task<int>? bound to a local yields a nullable result
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//!   The result type is always nullable
//! cites: 30.3
//! expect: run
//! issue: 7896
//! output: True
//!   False
//!   False
//!   True
//!   9
//!   True
//!   eq-true
//!   eq-null
module Conf;

public class Program
  public static method Main()
    using var t0 := Task.FromResult(1);
    var tp: Task? := t0;
    var tn: Task? := null;
    var a := tp?.IsCompleted;
    var b := tn?.IsCompleted;
    Console.WriteLine(a ?? false);
    Console.WriteLine(b ?? false);
    Console.WriteLine(a = null);
    Console.WriteLine(b = null);
    using var t1 := Task.FromResult(9);
    var t2: Task<int>? := t1;
    var r := t2?.Result;
    Console.WriteLine(r ?? -1);
    Console.WriteLine((tn?.IsCompleted ?? tp?.IsCompleted) ?? false);
    if tp?.IsCompleted = true then
      Console.WriteLine("eq-true");
    end if;
    if tn?.IsCompleted = null then
      Console.WriteLine("eq-null");
    end if;
  end Main;
end Program;
