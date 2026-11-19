//! case: s28_9_7716
//! doc: spec
//! section: 28.9
//! title: an immediately invoked lambda's closure result can itself be invoked at once
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 3
module Conf;
import System;
public class C
  public static method Main(): int
    Console.WriteLine((lambda(): () -> int => lambda() => 3)()());
    return 0;
  end Main;
end C;
