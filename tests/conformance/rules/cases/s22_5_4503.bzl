//! case: s22_5_4503
//! doc: spec
//! section: 22.5
//! title: named arguments to int.Parse and Debug.Assert bind inside generic methods
//!   and generic classes
//! rule: Named arguments may appear in any order after positional arguments.
//! cites: 22.6, 14.5
//! expect: run
//! output: 255
//!   255
//!   255
//! issue: 7602
module Conf;
import System.Diagnostics;

class G<T>
  public method P(): int
    Debug.Assert(message := "gm", condition := true);
    return int.Parse(radix := 16, s := "ff");
  end P;
end G;

class C
  public static method Gen<T>(x: T): int
    Debug.Assert(message := "gen-msg", condition := true);
    return int.Parse(radix := 16, s := "ff");
  end Gen;
  public static method Main(): int
    var g := new G<int>();
    Console.WriteLine(g.P());
    Console.WriteLine(Gen<int>(1));
    Console.WriteLine(Gen<string>("a"));
    return 0;
  end Main;
end C;
