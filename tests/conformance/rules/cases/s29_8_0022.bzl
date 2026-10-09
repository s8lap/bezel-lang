//! case: s29_8_0022
//! doc: spec
//! section: 29.8
//! title: the same iterator without the adoption carries the must-use warning
//! rule: If the body does not move ownership out, the one enumerator disposes the
//!   parameter when it completes or is disposed ... This implicit frame disposal
//!   carries the §28.3 must-use warning, attached at the parameter declaration.
//! cites: 29.8, 28.3
//! expect: warn
//! diagnostic: must use
module Conf;

public class LineSource
  private _text: string;
  private _n: int := 0;

  public constructor(text: string)
    _text := text;
  end constructor;

  public property AtEnd: bool => _n >= 1;

  public method ReadLine(): string
    _n := _n + 1;
    return _text;
  end ReadLine;

  destructor
    Console.WriteLine("source closed");
  end destructor;
end LineSource;

public class Reader
  public method Echo(owned source: LineSource): owned IEnumerable<string>
    while not source.AtEnd do
      yield return source.ReadLine();
    end while;
  end Echo;
end Reader;
