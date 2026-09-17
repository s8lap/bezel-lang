//! case: s16_3_0004
//! doc: spec
//! section: 16.3
//! title: iteration over Range<char> steps across the surrogate gap
//! rule: "Iteration steps across the gap: the successor of U+D7FF is U+E000 and the
//!   predecessor of U+E000 is U+D7FF... `'ퟰ'..'' step 4` yields U+D7F0,
//!   U+D7F4, U+D7F8, U+D7FC, then U+E000 (the fourth successor of U+D7FC), then
//!   U+E004, U+E008, U+E00C, U+E010. No surrogate is ever yielded."
//! cites: 16.3
//! expect: run
//! output: 55280
//!   55284
//!   55288
//!   55292
//!   57344
//!   57348
//!   57352
//!   57356
//!   57360
module Conf;

public class Program
  public static method Main()
    var r: Range<char> := 'ퟰ'..'' step 4;
    for var c in r do
      Console.WriteLine(int(c));
    end;
  end Main;
end Program;
