//! case: s22_3_7738
//! doc: spec
//! section: 22.3
//! title: calling a closure at an owned slot with an unmarked owned parameter is rejected
//! rule: Omitting the marker on a named owned binding is a compile error naming the fix
//! cites: 28.8
//! expect: reject
//! diagnostic: consumes
module Conf;
import System;
public class Res
  public constructor() end constructor;
  destructor
    Console.WriteLine("gone");
  end destructor;
end Res;
public class Program
  public static method Go(owned a: Res): int
    var f := lambda(owned r: Res): int => do r.Dispose(); return 1; end;
    return f(a);
  end Go;
end Program;
