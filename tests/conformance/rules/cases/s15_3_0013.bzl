//! case: s15_3_0013
//! doc: spec
//! section: 15.3
//! title: a move-only local's owned slots are auto-disposed at scope end in reverse slot order
//! rule: A move-only local's owned slots are auto-disposed at scope end by the
//!   compiler, in reverse slot order (§15.3, §15.1.1).
//! cites: 15.3, 15.1.1
//! expect: run
//! output: first
//!   dispose second
//!   dispose first
//!   after
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;

  destructor
    Console.WriteLine($"dispose {Tag}");
  end destructor;
end Widget;

public class Program
  public static method Main()
    do
      var a: [2 owned]Widget := [new Widget("first"), new Widget("second")];
      Console.WriteLine(a[0].Tag);
    end;
    Console.WriteLine("after");
  end Main;
end Program;
