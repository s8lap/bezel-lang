//! case: s28_3_8_3602
//! doc: spec
//! section: 28.3.8
//! title: a function-typed ref parameter's entry value passed at an Object parameter is rejected
//! rule: A read carrying the unannounced source may be invoked, bound to a local or passed at a plain
//!   function-typed parameter, but not stored into a holder (the `ref` or `out` location itself and a
//!   sibling one included), passed at a set-erasing position or returned from the enclosing method.
//! cites: 28.3.8, 22.3, 28.9
//! expect: reject
//! diagnostic: unannounced
//! issue: 9093
module Conf;

public class Reg2
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
end Reg2;

public class Program
  public static method KeepRef(rg: Reg2, ref b: () -> int)
    rg.Keep(b);
  end KeepRef;
end Program;
