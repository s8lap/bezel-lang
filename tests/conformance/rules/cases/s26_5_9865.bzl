//! case: s26_5_9865
//! doc: spec
//! section: 26.5
//! title: An alias of an interface does not supply the ReferenceEquals static
//! rule: a call or read `T.M` that none of them supplies names no member and is the same compile error
//! cites: 26.5, 8.1, 11.2, 27.8
//! expect: reject
//! diagnostic: no member
//! issue: 9021
//! notes: Harvested from the #9021 review (r13_alias_interface). The alias sees through to the interface (27.8); 11.2 gives an interface no ReferenceEquals.
module Conf;
public interface I end I;
type IA := I;

public class Program
  public static method Main(): int
    var i: I? := null;
    Console.WriteLine(IA.ReferenceEquals(i, i));
    return 0;
  end Main;
end Program;
