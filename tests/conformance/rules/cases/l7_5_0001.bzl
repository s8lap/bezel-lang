//! case: l7_5_0001
//! doc: stdlib
//! section: 7.5
//! title: catching FileNotFoundException requires `import System.IO;`
//! rule: None are auto-imported; users import System.IO; to get them.
//! cites: 7.5
//! expect: accept
module Conf;

import System.IO;

public class Program
  public method Guard()
    try
      DoWork();
    catch
      FileNotFoundException => Console.WriteLine("nf")
    end;
  end Guard;

  private method DoWork()
  end DoWork;
end Program;
