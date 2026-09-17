//! case: s29_8_0021
//! doc: spec
//! section: 29.8
//! title: the using var move adoption is the iterator's warning-free spelling
//! rule: This implicit frame disposal carries the §28.3 must-use warning, attached
//!   at the parameter declaration ...; the `using var … := move …` adoption above
//!   is the warning-free spelling.
//! cites: 29.8, 28.3, 28.10
//! expect: quiet
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
  public method ReadAndClose(owned source: LineSource): owned IEnumerable<string>
    using var r := move source;
    while not r.AtEnd do
      yield return r.ReadLine();
    end while;
  end ReadAndClose;
end Reader;
