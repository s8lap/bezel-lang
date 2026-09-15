//! case: s29_8_0001
//! doc: spec
//! section: 29.8
//! title: an iterator's owned parameter is discharged via using var move on entry
//! rule: The body may treat the `owned` parameter as it would in any other
//!   method. Using `using var … := move …` — the binding-position transfer
//!   marker — makes scope-bounded disposal explicit; the marked transfer
//!   moves ownership from the parameter to the local.
//! cites: 29.8
//! expect: run
//! stdout: ab
module Conf;

public class Lines
  private _a: string;
  private _b: string;
  private _i: int := -1;

  public constructor(a: string, b: string)
    _a := a;
    _b := b;
  end constructor;

  public property AtEnd: bool => _i >= 1;

  public method ReadLine(): string
    _i := _i + 1;
    return (case _i of 0 => _a, _ => _b);
  end ReadLine;
end Lines;

public class Program
  public static method ReadAndClose(owned reader: Lines): owned IEnumerable<string>
    using var r := move reader;
    while not r.AtEnd do
      yield return r.ReadLine();
    end;
  end ReadAndClose;

  public static method Main()
    var sb := new StringBuilder();
    for var line in ReadAndClose(new Lines("a", "b")) do
      sb.Append(line);
    end;
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
