//! case: s28_9_7720
//! doc: spec
//! section: 28.9
//! title: reassigning a function-typed local through an alias leaves the alias's closure valid
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 21
module Conf;
import System;
public class C
  public static method Main(): int
    var c := lambda() => 7;
    var g := lambda(): () -> int => c;
    var f := lambda() => 1;
    var a := g();
    f := g();
    a := g();
    f := a;
    Console.WriteLine(f() + a() + c());
    return 0;
  end Main;
end C;
