//! case: s15_1_1206
//! doc: spec
//! section: 15.1
//! title:ElementAtOrNull on a lazy Where over a record wrapping an inline array is admitted
//! rule:a standard-library member whose **signature** forms `T?` at a type parameter of its
//!   receiver type
//! cites: 14.2
//! expect: run
//! stdout: some
module Conf;

public record R(arr: [3]int);

public class Program
  public static method Main()
    using var xs := new Array<R>();
    xs.Add(new R([1, 2, 3]));
    var v := xs.Where(lambda(x) => true).ElementAtOrNull(0);
    if v <> null then
      Console.WriteLine("some");
    else
      Console.WriteLine("none");
    end if;
  end Main;
end Program;
