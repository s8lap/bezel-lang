//! case: s20_0002
//! doc: spec
//! section: 20
//! title: an elif chain performs multi-way selection, stopping at the first true condition
//! rule: "Multi-way selection chains with `elif`, in the statement form's word
//!   order" — the spec's own grade example: score 85 selects the "B" branch.
//! cites: 20
//! expect: run
//! stdout: B
module Conf;

public class Program
  public static method Main()
    var score := 85;
    var grade := (if score >= 90 then "A"
                  elif score >= 80 then "B"
                  elif score >= 70 then "C"
                  else "F");
    Console.WriteLine(grade);
  end Main;
end Program;
