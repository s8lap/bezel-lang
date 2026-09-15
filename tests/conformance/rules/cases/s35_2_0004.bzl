//! case: s35_2_0004
//! doc: spec
//! section: 35.2
//! title: override ToString is admitted on a choice
//! rule: §10.5's Equals/GetHashCode/ToString replacement rule carries over for the
//!   boxable classifications, spelled override or plain.
//! expect: run
//! stdout: ready
module Conf;

public choice Outcome
  Ready,
  Empty;

  public override method ToString(): string => (case this of Ready => "ready", Empty => "empty");
end Outcome;

public class Program
  public static method Main()
    var o := Outcome.Ready;
    Console.WriteLine(o.ToString());
  end Main;
end Program;
