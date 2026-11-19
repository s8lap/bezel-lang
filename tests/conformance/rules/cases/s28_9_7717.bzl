//! case: s28_9_7717
//! doc: spec
//! section: 28.9
//! title: an immediately invoked lambda that returns a captured closure leaves the source closure usable
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 6
module Conf;
import System;
public class C
  public static method Main(): int
    var h := lambda() => 3;
    var f := (lambda(): () -> int => h)();
    Console.WriteLine(f() + h());
    return 0;
  end Main;
end C;
