//! case: s15_1_1_0011
//! doc: spec
//! section: 15.1.1
//! title: after the restoration store the binding reads the replacement value, visible whole
//! rule: A whole-value `move` of an owning-inline-array **field** ... leaves
//!   the field's storage in §9.4's disposed state until the restoration
//!   store (§15.1.1); the restoration store, like a whole-value overwrite,
//!   stores a replacement, visible whole (§9.4).
//! cites: 15.1.1, 9.4, 15.3
//! expect: run
//! stdout: False
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
        var empty: [1 owned]Conn?;
        var t := move h.Slots;
        h.Slots := empty;
        try
          Console.WriteLine(a[0] <> null);
        catch
          DisposedAccessException => Console.WriteLine("faulted"),
        end try;
      end
    end case;
  end Main;
end Program;
