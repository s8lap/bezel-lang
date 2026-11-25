//! case: s7_10_9004
//! doc: spec
//! section: 7.10
//! title: a closure returning a closure over T? is usable (nested function types over nullable T)
//! rule: `T?` always means "the nullable form of T," regardless of constraint.
//! cites: 14.1, 7.2, 27.5
//! expect: run
//! output: 19900
//!   lit
//! notes: Harvested from the #9120 review (b3). `() -> () -> T?` nests the nullable function return one level
//!   down. Trace: g() ?? -1 sums i over 0..199 = 19900; the string instantiation prints the captured
//!   literal.
module Conf;
public class Program
  public static method M<T>(p: T): () -> () -> T?
    return lambda(): () -> T? => lambda(): T? => p;
  end M;
  public static method Main(): int
    var tot := 0;
    var i := 0;
    while i < 200 do
      var f := Program.M<int>(i);
      var g := f();
      tot := tot + (g() ?? -1);
      i := i + 1;
    end;
    Console.WriteLine(tot);
    var s := Program.M<string>("lit");
    var sg := s();
    var sr := sg();
    Console.WriteLine(sr ?? "nul");
    return 0;
  end Main;
end Program;
