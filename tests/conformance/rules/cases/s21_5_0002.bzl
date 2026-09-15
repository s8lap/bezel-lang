//! case: s21_5_0002
//! doc: spec
//! section: 21.5
//! title: a labeled loop rejects the keyword closer
//! rule: Because the construct now has a name, the introducing-keyword
//!   closer (`end for;`, `end while;`) applies only to unlabeled loops —
//!   once labeled, the keyword closer is rejected.
//! cites: 21.5, 1.6
//! expect: reject
//! diagnostic: label
module Conf;

public class Program
  public static method Main()
    scan: for var i in 1..3 do
      Console.WriteLine(i.ToString());
    end for;
  end Main;
end Program;
