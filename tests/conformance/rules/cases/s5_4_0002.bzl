//! case: s5_4_0002
//! doc: spec
//! section: 5.4
//! title: an integer-to-enum cast of a constant equal to no declared constant is a compile error
//! rule: When n is a compile-time constant equal to no declared constant, the
//!   cast is a compile error at the cast site — the same posture as §5.1's
//!   constant-narrowing range error.
//! cites: 5.4
//! expect: reject
//! diagnostic: compile error at the cast site
module Conf;

public enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Make()
    var p := Priority(5);
  end Make;
end Program;
