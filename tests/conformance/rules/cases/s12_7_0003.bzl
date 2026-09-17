//! case: s12_7_0003
//! doc: spec
//! section: 12.7
//! title: a ref argument into a value-narrowed Object box is rejected
//! rule: A write whose store would land in the box is a compile error: such
//!   a chain passed as a ref or out argument, which §22.3's
//!   assignable-storage-location categories admit -- through which the
//!   callee's write reaches the payload.
//! cites: 12.7, 22.3
//! expect: reject
//! diagnostic: box
module Conf;

public class Program
  public static method Swap(ref x: int, ref y: int)
    var tmp := x;
    x := y;
    y := tmp;
  end Swap;

  public static method Test()
    var a: [3]int := [4, 5, 6];
    var o: Object := a;
    if o is [3]int then
      Swap(ref o[0], ref o[1]);
    end if;
  end Test;
end Program;
