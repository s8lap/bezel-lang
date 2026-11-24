//! case: s27_5_8001
//! doc: spec
//! section: 27.5
//! title: a nested lambda assigns a captured function local that its sibling then reads and
//!   returns
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!   vice versa.
//! cites: 27.5, 28.3
//! expect: run
//! stdout: 20100
//! notes: Pins the #3743 regression fix (the #8948 ancestor-merge verdict aborted with a
//!   closure over-release on this shape).
module Conf;
public class C
  public static method Make(n: int): () -> int
    var outer := lambda(): () -> int => do
      var f := lambda(): int => n;
      var g := lambda(): int => do
        f := lambda(): int => n + 1;
        return f();
      end;
      return g;
    end;
    return outer();
  end Make;
  public static method Run(n: int): int
    var g := C.Make(n);
    return g();
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + Run(j); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
