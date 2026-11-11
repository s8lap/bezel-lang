//! case: s9_7_4601
//! doc: spec
//! section: 9.7
//! title: returning a span over a collection-literal local is rejected at the return site
//! rule: a return carrying any other borrow is rejected at the return site
//! cites: 28.3.4, 15.4
//! expect: reject
//! diagnostic: 9.7 borrow-lifetime rule
module Conf;

public class Program
  public static method Make(): Span<int>
    var xs: Array<int> := [1, 2, 3];
    return xs.AsSpan();
  end Make;
  public static method Main()
    var s := Make();
    Console.WriteLine(s[0]);
  end Main;
end Program;
