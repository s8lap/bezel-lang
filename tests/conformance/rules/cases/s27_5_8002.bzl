//! case: s27_5_8002
//! doc: spec
//! section: 27.5
//! title: a doubly nested lambda's assignment to an outer function local is visible to a
//!   later read
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!   vice versa.
//! cites: 27.5
//! expect: run
//! stdout: 20500
//! notes: Pins the #3743 regression fix on a three-level nesting (the base aborted with a
//!   closure over-release).
module Conf;
public class C
  public static method Run(n: int): int
    var outer := lambda(): int => do
      var mid := lambda(): int => do
        var f := lambda(): int => n;
        var inner := lambda(): int => do f := lambda(): int => n + 3; return 0; end;
        var t := inner();
        return t + f();
      end;
      return mid();
    end;
    return outer();
  end Run;
  public static method Main(): int
    var total := 0;
    for var j in 0..<200 do total := total + Run(j); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
