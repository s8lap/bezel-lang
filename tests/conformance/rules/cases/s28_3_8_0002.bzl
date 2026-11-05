//! case: s28_3_8_0002
//! doc: spec
//! section: 28.3.8
//! title: storing a closure over an owned Array local in a function-typed field is rejected
//! rule: While its source set is non-empty, a function value may not be stored in a field
//! cites: 28.3.8
//! expect: reject
//! diagnostic: field
module Conf;

public class Keep
  public F: () -> int := lambda() => 0;

  public constructor()
  end constructor;

  public method Set(t: string)
    var a := t.Split(",");
    this.F := lambda() => a[1].Length;
  end Set;
end Keep;

public class Program
  public static method Main()
    var kp := new Keep();
    kp.Set("a,bc,dc");
    Console.WriteLine(kp.F());
  end Main;
end Program;
