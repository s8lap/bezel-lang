//! case: s7_10_9006
//! doc: spec
//! section: 7.10
//! title: a closure whose body is a conditional of T and null, returned as () -> T?, selects per the condition
//! rule: `T?` always means "the nullable form of T," regardless of constraint.
//! cites: 14.1, 7.2, 27.5
//! expect: run
//! output: 4
//!   -1
//!   lit
//!   nul
//! notes: Harvested from the #9120 review (b5). Control: the conditional-bodied twin of the #9120 shape
//!   compiles on the parent (base-green); it pins that the fix did not disturb it. Trace: f has c = true
//!   so yields 4; g has c = false so yields null, and -1 is the fallback; likewise "lit" and "nul".
module Conf;
public class Program
  public static method M<T>(p: T, c: bool): () -> T?
    return lambda(): T? => (if c then p else null);
  end M;
  public static method Main(): int
    var f := Program.M<int>(4, true);
    var g := Program.M<int>(4, false);
    Console.WriteLine(f() ?? -1);
    Console.WriteLine(g() ?? -1);
    var fs := Program.M<string>("lit", true);
    var gs := Program.M<string>("lit", false);
    Console.WriteLine(fs() ?? "nul");
    Console.WriteLine(gs() ?? "nul");
    return 0;
  end Main;
end Program;
