//! case: s28_12_0015
//! doc: spec
//! section: 28.12
//! title: await using outside an async method is a compile error
//! rule: Language spec §28.12: "`await using` is valid only inside an async
//!     method or async lambda." A `using var`-shaped `await using var` in a
//!     plain (non-async) method is therefore an invalid program, and the
//!     compiler must reject it.
//! cites: 28.12
//! expect: reject
//! diagnostic: async
//! issue: 7396
module Conf;

public class Program
  public static method Main()
    var o := 1;
    await using var t := o.ToString();
  end Main;
end Program;
