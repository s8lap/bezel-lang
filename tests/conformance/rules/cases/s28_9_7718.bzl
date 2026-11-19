//! case: s28_9_7718
//! doc: spec
//! section: 28.9
//! title: a block-bodied immediately invoked lambda returns a closure bound inside its body
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 3
module Conf;
import System;
public class C
  public static method Main(): int
    var f := (lambda(): () -> int => do var q := lambda() => 3; return q; end)();
    Console.WriteLine(f());
    return 0;
  end Main;
end C;
