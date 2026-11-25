//! case: s28_9_9863
//! doc: spec
//! section: 28.9
//! title: A closure given to an abstract member with a throwing iterator override is callable after the catch
//! rule: Function-typed fields, parameters, and locals are not subject to `owned` annotations and carry no disposal obligation
//! cites: 28.9, 28.14, 29.4
//! expect: run
//! stdout: 25400
//! notes: Harvested from the #9309 review (p19_throw_in_frame). An ABSTRACT member whose override is
//!   an iterator that throws after two yields; the consumer catches the exception, and the closure
//!   local passed to the member stays callable (the frame's reference ended with the throw, the
//!   caller's own did not). Trace per round: c = 42; the loop body adds 42 + 42 = 84 before the
//!   throw; the catch arm adds 1; g() adds 42; 127 per round, 200 rounds = 25400.
module Conf;

public abstract class Fl
  public abstract method M(q: () -> int): IEnumerable<int>;
end Fl;

public class Sub : Fl
  public override method M(q: () -> int): IEnumerable<int>
    yield return q();
    yield return q();
    throw new Exception("boom");
  end M;
end Sub;

public class Program
  public static method Main(): int
    var t := 0;
    var fl: Fl := new Sub();
    for var j in 0 ..< 200 do
      var c := 42;
      var g := lambda() => c;
      try
        for var x in fl.M(g) do t := t + x; end;
      catch Exception =>
        t := t + 1;
      end;
      t := t + g();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
