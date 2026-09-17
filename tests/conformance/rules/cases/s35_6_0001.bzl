//! case: s35_6_0001
//! doc: spec
//! section: 35.6
//! title: every declared case covered by an irrefutable arm needs no `_` arm
//! rule: Coverage is counted per case name, one level deep — a payload-free case by a
//!   constant-style pattern, a payload case by a destructuring whose every slot is
//!   irrefutable. When every case is covered, no _ arm is required.
//! expect: run
//! stdout: ok 7
module Conf;

public choice Outcome
  Ok(value: int),
  Error(message: string);
end Outcome;

public class Program
  public static method Main()
    var o := Outcome.Ok(7);
    case o of
      Ok(var v)    => Console.WriteLine($"ok {v}"),
      Error(var m) => Console.WriteLine(m)
    end case;
  end Main;
end Program;
