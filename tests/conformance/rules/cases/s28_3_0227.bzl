//! case: s28_3_0227
//! doc: spec
//! section: 28.3
//! title: a while condition's unbound owned temporary is disposed at each evaluation
//! rule: K8.2 (spec §28.3's kernel, v0.343): "Conditions and guards are
//!   per-evaluation boundaries: temporaries never accumulate across iterations";
//!   K8.1 (v0.343): "Unbound owned temporaries are disposed at their full
//!   expression's end, after their uses, in reverse production order"; and
//!   K8's own definition (v0.343) makes "each *evaluation* of a condition or
//!   guard" an enumerated position of a full expression — so each evaluation
//!   disposes the temporary it produced, and three evaluations dispose three.
//! cites: 28.3, C.3
//! expect: run
//! output: cond
//!   dispose
//!   cond
//!   dispose
//!   cond
//!   dispose
//!   3
//! issue: 7187
module Conf;

class Temp public
  constructor() end constructor;

  destructor
    Console.WriteLine("dispose");
  end destructor;
end Temp;

public class Program
  public static method Main(): int
    var n := 0;
    while Keep(new Temp()) do
      n := n + 1;
      if n >= 3 then break; end;
    end while;
    Console.WriteLine(n);
    return 0;
  end Main;

  static method Keep(t: Temp): bool
    Console.WriteLine("cond");
    return true;
  end Keep;
end Program;
