//! case: s27_3_0043
//! doc: spec
//! section: 27.3
//! title: a bare return is a procedure exit; the block lambda is a procedure lambda at () -> ()
//! rule: its exits are bare `return;` and falling off the end (v0.338 §27.3)
//! cites: 27.3
//! expect: run
//! output: a
module Conf;

public class Program
  public static method Main()
    var w: () -> () := lambda() => do Console.WriteLine("a"); return; end;
    w();
  end Main;
end Program;
