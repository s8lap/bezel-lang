//! case: s22_8_0019
//! doc: spec
//! section: 22.8
//! title: a constructor may copy a readonly inline array into a local, write the copy, and store it
//! rule: A value type is copied on assignment; each variable holds an independent value.
//! cites: 22.8, 3.2
//! expect: run
//! stdout: 5
module Conf;

public class Holder
  public readonly A: [2]int;

  public constructor()
    var c := this.A;
    c[0] := 5;
    this.A := c;
  end constructor;
end Holder;

public class Program
  public static method Main()
    var h := new Holder();
    Console.WriteLine(h.A[0]);
  end Main;
end Program;
