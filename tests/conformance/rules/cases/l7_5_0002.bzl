//! case: l7_5_0002
//! doc: stdlib
//! section: 7.5
//! title: FileNotFoundException is unresolved without `import System.IO;`
//! rule: None are auto-imported; users import System.IO; to get them.
//! cites: 7.5
//! expect: reject
//! diagnostic: FileNotFoundException
module Conf;

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
