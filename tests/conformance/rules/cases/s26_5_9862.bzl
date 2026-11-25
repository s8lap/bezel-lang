//! case: s26_5_9862
//! doc: spec
//! section: 26.5
//! title: A choice type name does not supply the ReferenceEquals static
//! rule: a call or read `T.M` that none of them supplies names no member and is the same compile error
//! cites: 26.5, 8.1, 11.2, 27.8
//! expect: reject
//! diagnostic: no member
//! issue: 9021
//! notes: Harvested from the #9021 review (r08_choice). A choice reaches Object by boxing (8.1, 35.8); its type name supplies no ReferenceEquals (26.5 rule 2).
module Conf;
public choice Ch Left(v: int), Right; end Ch;

public class Program
  public static method Main(): int
    Console.WriteLine(Ch.ReferenceEquals(Ch.Right, Ch.Right));
    return 0;
  end Main;
end Program;
