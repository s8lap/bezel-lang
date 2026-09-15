//! case: s17_11_0003
//! doc: spec
//! section: 17.11
//! title: default(char) is the null character
//! rule: "`char`: `'\\0'`."
//! cites: 17.11
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var c := default(char);
    Console.WriteLine(c = '\0');
  end Main;
end Program;
