//! case: s15_4_0010
//! doc: spec
//! section: 15.4
//! title: a literal mixing bare expressions and colon pairs is a compile error
//! rule: The sequence and dictionary literal forms are syntactically
//!   distinguished by whether the elements contain a top-level colon; a literal
//!   cannot mix bare expressions and colon pairs (doing so is a compile error)
//!   (§15.4).
//! cites: 15.4
//! expect: reject
//! diagnostic: mix
module Conf;

public class Program
  public static method Run()
    using var ages: Dictionary<string, int> := ["alice": 30, 25];
    Console.WriteLine(ages.Count);
  end Run;
end Program;
