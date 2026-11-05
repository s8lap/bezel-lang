//! case: s28_3_8_1202
//! doc: spec
//! section: 28.3.8
//! title:a capture-free lambda is stored in a function-typed field beside an unrelated owned Array
//!   local
//! rule:While its source set is non-empty, a function value may not be stored in a field
//! expect: run
//! stdout: 2
module Conf;

public class Keep
  public F: () -> int := lambda() => 0;

  public constructor()
  end constructor;

  public method Set(t: string)
    var a := t.Split(",");
    this.F := lambda() => 2;
  end Set;
end Keep;

public class Program
  public static method Main()
    var kp := new Keep();
    kp.Set("a,bc,dc");
    Console.WriteLine(kp.F());
  end Main;
end Program;
