//! case: s29_8_0002
//! doc: spec
//! section: 29.8
//! title: an undischarged owned iterator parameter carries the must-use warning
//! rule: If the body does not move ownership out, the state machine
//!   disposes the parameter at iterator disposal ... This implicit frame
//!   disposal carries the §28.3 must-use warning, attached at the parameter
//!   declaration.
//! cites: 29.8, 28.3
//! expect: warn
//! diagnostic: must use
module Conf;

public class Lines
  private _a: string;
  private _i: int := -1;

  public constructor(a: string)
    _a := a;
  end constructor;

  public property AtEnd: bool => _i >= 0;

  public method ReadLine(): string
    _i := _i + 1;
    return _a;
  end ReadLine;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Lines;

public class Program
  public static method Echo(owned source: Lines): owned IEnumerable<string>
    while not source.AtEnd do
      yield return source.ReadLine();
    end;
  end Echo;
end Program;
