//! case: s28_9_0009
//! doc: spec
//! section: 28.9
//! title: an undischarged plain-var local of a trivial-dispose class draws no must-use warning
//! rule: No must-use / auto-drop diagnostic attaches to a trivial-dispose
//!   value: the §28.3 must-use warning is not emitted at the implicit-drop site
//!   of a plain-`var` owned local whose type is trivial-dispose — a class with
//!   no destructor and no `owned` fields of non-trivial-dispose type (§28.9).
//! cites: 28.9, 28.3
//! expect: quiet
module Conf;

public class Ticket
  public property Id: int;

  public constructor(id: int)
    Id := id;
  end constructor;
end Ticket;

public class Program
  public static method Run()
    var t := new Ticket(7);
    Console.WriteLine(t.Id);
  end Run;
end Program;
