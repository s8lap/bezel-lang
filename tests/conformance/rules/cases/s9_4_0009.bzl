//! case: s9_4_0009
//! doc: spec
//! section: 9.4
//! title: the same slot read through the binding with the holder alive reads the slot
//! rule: A move-only value disposed in place **with no replacement** — ... or
//!   the holder of either kind destroyed (§28.4 step 2 ...) — is left in a
//!   **disposed state**
//! cites: 9.4, 15.1.1, 19.3
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
      end
    end case;
  end Main;
end Program;
