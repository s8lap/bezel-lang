//! case: s30_6_0003
//! doc: spec
//! section: 30.6
//! title: an async lambda may not target a function type with a ref slot
//! rule: The rule extends to conversions: an `async` lambda may not be
//!   assigned to a function type (§27.1) whose parameter list carries a
//!   `ref`/`out` slot.
//! cites: 30.6, 30.1
//! expect: reject
//! diagnostic: ref
module Conf;

public class Program
  public method Setup()
    var f: (ref int) -> Task := async lambda(ref x) => do
      await Task.Delay(0);
    end;
  end Setup;
end Program;
