//! case: l2_5_0050
//! doc: stdlib
//! section: 2.5
//! title: new TimeSpan(...) is rejected: TimeSpan declares no constructor
//! rule: No constructor, so `new TimeSpan(...)` is rejected (spec §17.7's closed
//!   constructor surface): a span comes from FromTicks, a From<Unit> factory,
//!   Zero/MinValue/MaxValue, or a member or operator that returns one.
//! cites: 2.5
//! expect: reject
//! diagnostic: constructor
//! issue: 7682
module Conf;

public class Program
  public static method Make(): TimeSpan => new TimeSpan(1);
end Program;
