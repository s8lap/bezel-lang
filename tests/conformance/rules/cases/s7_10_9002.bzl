//! case: s7_10_9002
//! doc: spec
//! section: 7.10
//! title: a closure returned as () -> T? over an unconstrained T is usable at a record instantiation
//! rule: `T?` always means "the nullable form of T," regardless of constraint.
//! cites: 14.1, 7.2, 27.5
//! expect: run
//! output: 20300
//! notes: Harvested from the #9120 review (a2). T = Pt (a positional record, a value type): `T?` is `Pt?`.
//!   Trace: per iteration r!.X + r!.Y = i + 2; sum over 0..199 = 19900 + 400 = 20300.
module Conf;
public record Pt(X: int, Y: int);
public class Program
  public static method M<T>(p: T): () -> T?
    return lambda(): T? => p;
  end M;
  public static method Main(): int
    var tot := 0;
    var i := 0;
    while i < 200 do
      var f := Program.M<Pt>(new Pt(i, 2));
      var r := f();
      if r <> null then tot := tot + r!.X + r!.Y; end if;
      i := i + 1;
    end;
    Console.WriteLine(tot);
    return 0;
  end Main;
end Program;
