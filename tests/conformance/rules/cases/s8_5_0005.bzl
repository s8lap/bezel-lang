//! case: s8_5_0005
//! doc: spec
//! section: 8.5
//! title: An Object holding a StringBuilder renders its text, then the replacement on the next pass
//! rule: `ToString` is invoked via virtual dispatch for present values; a class
//!   instance held at an `Object` location renders through its own `ToString`,
//!   and after the location is reassigned it renders the new value.
//! cites: 8.5, 8.1
//! expect: run
//! output: sbtext
//!   other
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder();
    _ := sb.Append("sbtext");
    var o: Object := sb;
    var i := 0;
    while i < 2 do
      Console.WriteLine(o);
      o := "other";
      i := i + 1;
    end while;
  end Main;
end Program;
