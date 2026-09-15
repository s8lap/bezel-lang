//! case: s28_9_0010
//! doc: spec
//! section: 28.9
//! title: adding a destructor makes the same undischarged local draw the must-use warning
//! rule: The ceremony exemption's boundary is exactly the runs-no-user-code
//!   line: a class that declares a destructor is not trivial-dispose, so the
//!   §28.3 must-use warning is emitted at the implicit-drop site of its
//!   plain-`var` owned local (§28.9, §28.3).
//! cites: 28.9, 28.3
//! expect: warn
//! diagnostic: must use
module Conf;

public class Ticket
  public property Id: int;

  public constructor(id: int)
    Id := id;
  end constructor;

  destructor
    Console.WriteLine("released");
  end destructor;
end Ticket;

public class Program
  public static method Run()
    var t := new Ticket(7);
    Console.WriteLine(t.Id);
  end Run;
end Program;
