//! case: s9_4_0008
//! doc: spec
//! section: 9.4
//! title: a match binding of an owning inline array field read after its holder's disposal is rejected
//! rule: or a local bound from a member read or call through the variable or through such a holder
//!   (`var v := a.F;`, `var v := a.Get();`, a borrowing-match binding over `a.F`), which §28.3's
//!   receiver-sourced rows source at the variable's value whether or not the field is `owned` … a use of
//!   the holder — a read or member access through it … — is the same compile error
//! cites: 28.2, 9.4, 15.1.1, 28.4
//! expect: reject
//! diagnostic: dominat
//! issue: 5642
//! notes: v0.361 (#8708) flips this case from run/faulted to reject: `h.Dispose()` dominates the read
//!   of `a[0]`, and `a` is a borrowing-match binding over `h.Slots`. The runtime fault it pinned is now
//!   reached only by a kill on some paths (proposal 0205's table, which already expected reject).
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
