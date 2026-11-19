//! case: s28_9_7721
//! doc: spec
//! section: 28.9
//! title: a function-typed local reassigned from a nested immediately invoked lambda in a loop
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 3
module Conf;
import System;
public class C
  public static method Main(): int
    var f := lambda() => 1;
    for var k in 0..<3 do f := (lambda(): () -> int => lambda() => 3)(); end;
    Console.WriteLine(f());
    return 0;
  end Main;
end C;
