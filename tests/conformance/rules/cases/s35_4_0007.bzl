//! case: s35_4_0007
//! doc: spec
//! section: 35.4
//! title: an owning-choice local under plain var auto-disposes at scope end and warns
//! rule: Owning choices are move-only value types; locals are auto-disposed at
//!   scope end with the must-use warning (K3.14). Disposal is defined by the
//!   active case: it disposes the active case's owned payloads.
//! cites: 15.3, 28.3
//! expect: run
//! warns: yes
//! output: scope end next
//!   disposed
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Resource;

public choice Holding
  Held(owned res: Resource),
  Nothing;
end Holding;

public class Program
  public static method Main()
    var h := Holding.Held(new Resource());
    Console.WriteLine("scope end next");
  end Main;
end Program;
