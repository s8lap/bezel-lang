//! case: s15_1_1_0010
//! doc: spec
//! section: 15.1.1
//! title: the same binding read before the whole-value move of the field reads the slot
//! rule: A whole-value `move` of an owning-inline-array **field** (`var t :=
//!   move h.Slots;`) ... leaves the field's storage in §9.4's disposed state
//!   until the restoration store
//! cites: 15.1.1, 9.4, 15.3
//! expect: run
//! stdout: True
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
    case h.Slots of
      var a => do
        try
          Console.WriteLine(a[0] <> null);
        catch
          DisposedAccessException => Console.WriteLine("faulted"),
        end try;
        var t := move h.Slots;
        h.Slots := t;
      end
    end case;
  end Main;
end Program;
