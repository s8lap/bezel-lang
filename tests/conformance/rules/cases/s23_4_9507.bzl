//! case: s23_4_9507
//! doc: spec
//! section: 23.4
//! title: function-typed indexer overloads called with capturing lambdas in a loop select by return type
//! rule: the candidate is applicable at the position when the lambda's inferred return type equals the
//!   candidate's return slot, implicitly converts to it
//! cites: 23.4, 22.6, 28.14
//! expect: run
//! output: 45750
//! notes: Pins #9187 (pick 3715595a8): an indexer overloaded only by a function-typed parameter binds the
//!   overload whose function type matches the argument's (sema and codegen agreed on the
//!   first-declared overload before the fix). Per iteration j: the int lambda selects the int getter
//!   (j + 1), the string lambda the string getter (2). Sum of (j + 1) for j = 0..299 is 45150, plus
//!   600 = 45750.
module Conf;

public class Box
  public v: int;
  public constructor(x: int)
    v := x;
  end constructor;
end Box;

public class C
  public property [f: () -> string]: int
    get => do return f().Length; end;
  end;
  public property [f: () -> int]: int
    get => do return f() + 1; end;
  end;
  public constructor()
  end constructor;
end C;

public class Program
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 300 do
      using var c := new C();
      using var b := new Box(j);
      t := t + c[lambda() => b.v];
      t := t + c[lambda() => "ab"];
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
