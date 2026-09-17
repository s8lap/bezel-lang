//! case: s16_3_0002
//! doc: spec
//! section: 16.3
//! title: a closed range's Start and End are non-null, and the default Step is 1
//! rule: "`Start` and `End` are nullable; a `null` endpoint indicates the range is
//!   open-ended on that side. For closed ranges (both endpoints present), both are
//!   non-null." ... "The default step is 1."
//! cites: 16.3
//! expect: run
//! stdout: 3,7,1,False
module Conf;

public class Program
  public static method Main()
    var r := 3..7;
    Console.WriteLine($"{r.Start},{r.End},{r.Step},{r.IsOpenEnded}");
  end Main;
end Program;
