//! case: s28_3_8_3605
//! doc: spec
//! section: 28.3.8
//! title: a function-typed ref parameter overwritten capture-free in the callee may then be set-erased
//! rule: the entry value, which this body cannot trace, contributes an **unannounced** source on every
//!   path no assignment of the body has reached, and each assignment of the body its own set
//! cites: 28.3.8, 22.3, 28.9
//! expect: accept
//! issue: 9093
//! notes: The read of `a` after `a := lambda() => 9;` carries that assignment's empty set, not the
//!   unannounced source, so passing it at `Keep`'s `Object` parameter is admitted (contrast s28_3_8_3602).
module Conf;

public class Reg2
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
end Reg2;

public class Program
  public static method Reset(rg: Reg2, ref a: () -> int)
    a := lambda() => 9;
    rg.Keep(a);
  end Reset;
end Program;
