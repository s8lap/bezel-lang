//! case: s27_5_7632
//! doc: spec
//! section: 27.5
//! title: a returned lambda that throws after forming a local lambda is caught by its caller
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 31.1
//! expect: run
//! stdout: 200
module Conf;
import System;
public class C
  public static method Make(): () -> int
    return lambda() => do
      var k := 3;
      var h := lambda() => k;
      if h() = 3 then throw new InvalidOperationException("boom"); end;
      return h();
    end;
  end Make;
  public static method Main(): int
    var total := 0;
    for var j in 0 ..< 200 do
      var g := C.Make();
      try total := total + g(); catch var e: InvalidOperationException => total := total + 1; end try;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
