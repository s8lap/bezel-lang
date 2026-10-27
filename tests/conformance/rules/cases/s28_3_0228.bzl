//! case: s28_3_0228
//! doc: spec
//! section: 28.3
//! title: an if condition's unbound owned temporary is disposed at that evaluation's end
//! rule: K8.1 (spec §28.3's kernel, v0.343): "Unbound owned temporaries are
//!   disposed at their full expression's end, after their uses, in reverse
//!   production order"; and K8's own definition (v0.343) makes "each
//!   *evaluation* of a condition or guard" an enumerated position of a full
//!   expression — so the condition's evaluation is its own boundary, and the
//!   temporary it produced is disposed there, after the call that uses it.
//! cites: 28.3, C.3
//! expect: run
//! output: cond
//!   dispose
//!   1
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
    if Keep(new Temp()) then
      n := 1;
    end if;
    Console.WriteLine(n);
    return 0;
  end Main;

  static method Keep(t: Temp): bool
    Console.WriteLine("cond");
    return true;
  end Keep;
end Program;
