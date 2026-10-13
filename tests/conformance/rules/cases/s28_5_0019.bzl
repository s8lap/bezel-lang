//! case: s28_5_0019
//! doc: spec
//! section: 28.5
//! title: owned on an owning-choice field is admitted but redundant, and the cascade disposes the payload once
//! rule: On a field of **owning-choice** type (§35.4 rule 1, a move-only value
//!   type) the modifier is **admitted-but-redundant**, as §28.1's type
//!   requirement reads a move-only value type at every marked position: the
//!   field is owned-bearing with or without it
//! cites: 28.5, 15.3, 28.4
//! expect: run
//! output: closed
//!   after
module Conf;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;

public choice Slot
  Held(owned r: Res),
  Empty;
end Slot;

public class Host
  private owned _slot: Slot := Slot.Held(new Res());
end Host;

public class Program
  public static method Main()
    do
      using var h := new Host();
    end;
    Console.WriteLine("after");
  end Main;
end Program;
