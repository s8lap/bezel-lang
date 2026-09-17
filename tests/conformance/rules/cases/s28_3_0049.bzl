//! case: s28_3_0049
//! doc: spec
//! section: 28.3
//! title: a do end block disposes a using var early, before the outer scope continues
//! rule: A `using var` local is owned and additionally subject to scope-based
//!   disposal at the end of the enclosing block; a `do … end` block is such a
//!   block, so the value is disposed at its end, not at the method's.
//! cites: 28.10
//! expect: run
//! output: disposed
//!   after block
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    do
      using var w := new Widget();
    end;
    Console.WriteLine("after block");
  end Main;
end Program;
