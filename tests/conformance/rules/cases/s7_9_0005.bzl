//! case: s7_9_0005
//! doc: spec
//! section: 7.9
//! title: a non-defaultable local assigned before its read is admitted
//! rule: A local of non-defaultable type must be assigned before it is read;
//!   once assigned, a read is admitted.
//! cites: 7.9, 17.11
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
