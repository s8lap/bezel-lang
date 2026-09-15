//! case: s7_9_0005
//! doc: spec
//! section: 7.9
//! title: a non-nullable local assigned before its read is admitted
//! rule: A non-nullable local must be assigned before it is read; once
//!   assigned, a read is admitted.
//! expect: run
//! stdout: assigned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var w: Widget;
    w := new Widget();
    var w2 := w;
    Console.WriteLine("assigned");
  end Main;
end Program;
