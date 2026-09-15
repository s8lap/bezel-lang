//! case: s20_0004
//! doc: spec
//! section: 20
//! title: only the selected branch is evaluated — an unselected throw branch's operand never runs
//! rule: "each condition and branch is a full expression with no precedence
//!   restriction — in particular, a `throw` branch is admitted in any branch
//!   position" together with "only the selected branch is evaluated." Since the
//!   `then` branch is selected here, the `else` branch's `new Probe()` — whose
//!   constructor is observable — never executes.
//! cites: 20
//! expect: run
//! output: 1
module Conf;

public class Probe
  public constructor()
    Console.WriteLine("built");
  end constructor;

  public override method ToString(): string => "probe";
end Probe;

public class Program
  public static method Main()
    var c := true;
    var v := (if c then 1 else throw new InvalidOperationException(new Probe().ToString()));
    Console.WriteLine(v);
  end Main;
end Program;
