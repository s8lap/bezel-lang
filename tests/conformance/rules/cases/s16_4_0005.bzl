//! case: s16_4_0005
//! doc: spec
//! section: 16.4
//! title: a constant range whose direction disagrees with its step's sign warns that iteration is empty
//! rule: "When the compiler can prove at compile time that a range's direction and
//!   step sign disagree (both endpoints and step are constants or constant after
//!   folding), it emits a warning: 'this range produces an empty iteration.'"
//! cites: 16.4
//! expect: warn
//! diagnostic: empty iteration
module Conf;

public class Program
  public static method Main()
    for var i in 10..20 step -1 do
      Console.WriteLine(i);
    end for;
  end Main;
end Program;
