//! case: s27_5_8721
//! doc: spec
//! section: 27.5
//! title: an imported virtual indexer setter called with a lambda index argument that captures the
//!   receiver
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 23.4, 2.8
//! expect: run
//! stdout: 601
//! notes: Pins #9099 (imported setter). Each of the 200 stores runs `Tag := q() + value` = Tag + 3
//!   in the imported class: 1 + 600 = 601.
module Conf;
import LibA;
public class Program
  public static method Main(): int
    var fl := new LibA.FlA();
    for var j in 0 ..< 200 do
      fl[lambda() => fl.Tag] := 3;
    end for;
    Console.WriteLine($"{fl.Tag}");
    return 0;
  end Main;
end Program;
