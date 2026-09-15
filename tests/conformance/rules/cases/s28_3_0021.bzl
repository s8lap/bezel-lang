//! case: s28_3_0021
//! doc: spec
//! section: 28.3
//! title: using var y := move x is the adoption spelling
//! rule: `using var y := move x;` is the standard way to turn an owned local's
//!   obligation into scope-bound disposal.
//! expect: run
//! stdout: before end
module Conf;

public class Widget
  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    var w := new Widget();
    using var held := move w;
    Console.WriteLine("before end");
  end Main;
end Program;
