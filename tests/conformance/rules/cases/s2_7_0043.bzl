//! case: s2_7_0043
//! doc: spec
//! section: 2.7
//! title: an alias names only the aliased module's own types, not another module's
//! rule: The remainder then resolves against that module's public declarations
//! cites: 2.2, 2.7
//! expect: reject
//! diagnostic: has no public type
module Conf;
import System as st;

public class Program
  public static method Main()
    st.File.Exists("x");
    Console.WriteLine("after");
  end Main;
end Program;
