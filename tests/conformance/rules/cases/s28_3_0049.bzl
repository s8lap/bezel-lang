//! case: s28_3_0049
//! doc: spec
//! section: 28.3
//! title: a do end block disposes a using var early, before the outer scope continues
//! rule: To dispose earlier than the method's end, nest a `do … end` block; the block
//!   ends a scope early.
//! expect: run
//! stdout: disposed
module Conf;

public class Widget
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
