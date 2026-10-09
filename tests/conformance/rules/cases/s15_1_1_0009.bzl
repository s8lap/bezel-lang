//! case: s15_1_1_0009
//! doc: spec
//! section: 15.1.1
//! title: a binding read of an owning inline array field faults inside the field's restoration window
//! rule: A whole-value `move` of an owning-inline-array **field** (`var t :=
//!   move h.Slots;`) takes K9.2's restoration rule at the field (§15.3
//!   *Local lifetime*) and leaves the field's storage in §9.4's disposed
//!   state until the restoration store, so that a borrowing-match binding
//!   over the field reads the state, not the taker's slots, in checked
//!   disposal mode.
//! cites: 15.1.1, 9.4, 15.3, 28.8.1
//! expect: run
//! stdout: faulted
//! issue: 5652
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public class Holder
  public Slots: [1 owned]Conn?;

  public constructor()
    Slots := [new Conn()];
  end constructor;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    try
      case h.Slots of
        var a => do
          var t := move h.Slots;
          var n := a.Length;
          h.Slots := t;
          Console.WriteLine(n);
        end
      end case;
    catch
      DisposedAccessException => Console.WriteLine("faulted"),
    end try;
  end Main;
end Program;
