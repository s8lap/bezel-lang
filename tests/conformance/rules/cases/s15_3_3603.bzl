//! case: s15_3_3603
//! doc: spec
//! section: 15.3
//! title: a plain binding of a move-only field read is bounded by the field's holder
//! rule: its validity is bounded by the storage's holder (a use of it after a dominating disposal of the
//!   holder is §28.2's compile error)
//! cites: 15.3, 28.2
//! expect: reject
//! diagnostic: dominat
//! issue: 7612
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
  public static method Main()
    var h := new H();
    var x := h.F;
    h.Dispose();
    Console.WriteLine((case x of OC.Text(r) => 1, OC.Empty => 0));
  end Main;
end Program;
