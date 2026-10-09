//! case: s9_4_0010
//! doc: spec
//! section: 9.4
//! title: a binding read of an owning choice field faults inside its straight-line restoration window
//! rule: and so is the storage an **instance field** of either kind vacates
//!   under a whole-value `move` (`var t := move h.Slots;` ...), from the move
//!   until the restoration store replaces the value ... In checked disposal
//!   mode a read of the value in that state — through an in-place `this`, or
//!   through a borrowing-match binding of it ... — faults as
//!   `DisposedAccessException`
//! cites: 9.4, 15.3, 35.5, 28.8.1
//! expect: run
//! stdout: faulted
//! issue: 5652
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public choice Slot
  Full(owned conn: Conn),
  Empty;

  public method IsFull(): bool => (case this of Full(_) => true, Empty => false);
end Slot;

public class Holder
  public C: Slot;

  public constructor()
    C := Slot.Full(new Conn());
  end constructor;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    try
      case h.C of
        var c => do
          var t := move h.C;
          var full := (case c of Full(_) => true, Empty => false);
          h.C := t;
          Console.WriteLine(full);
        end
      end case;
    catch
      DisposedAccessException => Console.WriteLine("faulted"),
    end try;
  end Main;
end Program;
