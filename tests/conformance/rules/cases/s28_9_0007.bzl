//! case: s28_9_0007
//! doc: spec
//! section: 28.9
//! title: reading a slot through a closure's captured borrow of a move-only local is admitted
//! rule: Reads of slots through the captured borrow are admitted — the
//!   closure captures a borrow whose source is the local's enclosing scope.
//! cites: 28.9, 15.3
//! expect: run
//! stdout: pinged
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
    Console.WriteLine("pinged");
  end Ping;
end Widget;

public class Program
  public static method Main()
    var arr: [1 owned]Widget := [new Widget()];
    var pinger := lambda() => arr[0].Ping();
    pinger();
  end Main;
end Program;
