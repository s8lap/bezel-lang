//! case: s15_3_3601
//! doc: spec
//! section: 15.3
//! title: a plain binding of a move-only field read is a borrow, so passing it by value is rejected
//! rule: A binding initialized from a **storage read** — a field, property, indexer or slot read,
//!   `var x := h.F;` — is not such a binding read: the read lends …, so `x` is a **borrow** of the
//!   move-only value … and a consuming use of it … is rejected
//! cites: 15.3, 28.3
//! expect: reject
//! diagnostic: borrow
//! issue: 7612
//! notes: `Take`'s plain parameter of a move-only type is a by-value move (15.3 *Parameter passing*),
//!   a consuming use. Accepting twin s15_3_3602 reads the binding instead.
module Conf;

public class Res
  public constructor() end constructor;
  destructor end destructor;
end Res;

public choice OC Text(owned r: Res), Empty; end OC;

public class H
  public owned F: OC := OC.Text(new Res());
  public constructor() end constructor;
end H;

public class Program
  public static method Take(o: OC): int => 1;
  public static method Main()
    using var h := new H();
    var x := h.F;
    Console.WriteLine(Program.Take(x));
  end Main;
end Program;
