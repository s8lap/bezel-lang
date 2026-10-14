//! case: s7_4_0046
//! doc: spec
//! section: 7.4
//! title: a mutably captured local keeps its narrowing for a straight-line check-then-use
//! rule: Straight-line check-then-use of a captured local (`if x <> null then x.M(); end;` — no call *between* the test and the use) remains accepted. (v0.338 §7.4 line 910)
//! cites: 7.4, 27.5
//! expect: run
//! output: 5
module Conf;

public class Program
  public static method Probe(label: string?)
    var clear := lambda() => do label := null; end;
    var noisy := lambda() => do Console.WriteLine("noisy"); end;
    if label <> null then
      Console.WriteLine(label.Length);
    end if;
    clear();
  end Probe;

  public static method Main()
    Probe("gizmo");
  end Main;
end Program;
