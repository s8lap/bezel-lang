//! case: s28_9_7719
//! doc: spec
//! section: 28.9
//! title: a function-typed local reassigned from a closure-returning call in a loop keeps the last value
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 14
module Conf;
import System;
public class C
  public static method Main(): int
    var c := lambda() => 7;
    var g := lambda(): () -> int => c;
    var f := lambda() => 1;
    for var k in 0..<3 do f := g(); end;
    Console.WriteLine(f() + c());
    return 0;
  end Main;
end C;
