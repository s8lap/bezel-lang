//! case: s26_5_9863
//! doc: spec
//! section: 26.5
//! title: An interface type name does not supply the ReferenceEquals static
//! rule: a call or read `T.M` that none of them supplies names no member and is the same compile error
//! cites: 26.5, 8.1, 11.2, 27.8
//! expect: reject
//! diagnostic: no member
//! issue: 9021
//! notes: Harvested from the #9021 review (r09_interface). 11.2: an interface declares no member named GetType or ReferenceEquals, instance or static, so through its type name nothing supplies it (26.5 rule 2).
module Conf;
public interface I end I;

public class Program
  public static method Main(): int
    var i: I? := null;
    Console.WriteLine(I.ReferenceEquals(i, i));
    return 0;
  end Main;
end Program;
