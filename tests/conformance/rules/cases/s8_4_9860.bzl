//! case: s8_4_9860
//! doc: spec
//! section: 8.4
//! title: A class alias spelling of ReferenceEquals given a value-typed argument warns and compares boxes
//! rule: The compiler warns on any value-typed argument
//! cites: 8.1, 8.4, 8.6, 27.8
//! expect: run
//! warns: yes
//! stdout: False
//! issue: 9021
//! notes: Harvested from the #9021 review (r17_alias_valuearg_warn). `KA` is an alias of the class K, so
//!   `KA.ReferenceEquals` is the root's static (27.8, 11.7) and its arguments are boxed (8.1): the enum
//!   argument is a value-typed argument (the warning), and two fresh boxes compare unequal, so False.
module Conf;

public enum Color Red, Green; end Color;
public class K end K;
type KA := K;

public class Program
  public static method Main(): int
    Console.WriteLine(KA.ReferenceEquals(Color.Red, Color.Red));
    return 0;
  end Main;
end Program;
