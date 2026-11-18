//! case: s27_5_7631
//! doc: spec
//! section: 27.5
//! title: a returned lambda that forms per-iteration lambda locals computes the hand total
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 6000
module Conf;
import System;
public class C
  public static method Make(): () -> int
    return lambda() => do
      var s := 0;
      for var i in 0 ..< 5 do var k := i; var h := lambda() => k + 1; s := s + h(); end;
      return s;
    end;
  end Make;
  public static method Main(): int
    var total := 0;
    for var j in 0 ..< 200 do var g := C.Make(); total := total + g() + g(); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
