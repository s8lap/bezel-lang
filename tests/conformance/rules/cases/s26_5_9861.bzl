//! case: s26_5_9861
//! doc: spec
//! section: 26.5
//! title: An enum type name does not supply the ReferenceEquals static
//! rule: a call or read `T.M` that none of them supplies names no member and is the same compile error
//! cites: 26.5, 8.1, 11.2, 27.8
//! expect: reject
//! diagnostic: no member
//! issue: 9021
//! notes: Harvested from the #9021 review (r07_enum). An enum reaches Object by boxing (8.1); its type name resolves nested types, constants and statics (26.5 rule 2), and ReferenceEquals is none of them.
module Conf;
public enum E A, B; end E;

public class Program
  public static method Main(): int
    Console.WriteLine(E.ReferenceEquals(E.A, E.B));
    return 0;
  end Main;
end Program;
