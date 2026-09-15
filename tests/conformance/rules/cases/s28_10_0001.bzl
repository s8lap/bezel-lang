//! case: s28_10_0001
//! doc: spec
//! section: 28.10
//! title: a using var declared inside a loop body is disposed on a break exit
//! rule: Disposal happens on every exit path — normal completion, break,
//!   return, or exception.
//! cites: 28.10
//! expect: run
//! output: dispose loopvar
//!   dispose loopvar
//!   dispose loopvar
//!   after loop
module Conf;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Widget;

public class Program
  public static method Main()
    var i := 0;
    while i < 5 do
      using var w := new Widget("loopvar");
      if i = 2 then
        break;
      end;
      i := i + 1;
    end;
    Console.WriteLine("after loop");
  end Main;
end Program;
