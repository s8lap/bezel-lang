//! case: s9_4_0008
//! doc: spec
//! section: 9.4
//! title: a binding read of an owning inline array field faults after its holder is destroyed
//! rule: A move-only value disposed in place **with no replacement** — ... or
//!   the holder of either kind destroyed (§28.4 step 2, whose per-slot pass
//!   disposes an owning inline array's slots in place, §15.1.1) — is left in
//!   a **disposed state** ... In checked disposal mode a read of the value in
//!   that state — through an in-place `this`, or through a borrowing-match
//!   binding of it, an owning inline array's slot reads, iteration and
//!   compiler-provided operations (§15.1.2) included — faults as
//!   `DisposedAccessException`
//! cites: 9.4, 15.1.1, 28.4, 28.2
//! expect: run
//! stdout: faulted
//! issue: 5642
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
        h.Dispose();
        try
          Console.WriteLine(a[0] <> null);
        catch
          DisposedAccessException => Console.WriteLine("faulted"),
        end try;
      end
    end case;
  end Main;
end Program;
