//! case: s2_3_9902
//! doc: spec
//! section: 2.3
//! title: a user generic class named OwningBuffer keeps its own writable Length
//! rule: every other always-available name may be shadowed by local declarations under the normal
//!   name-resolution rules
//! cites: 2.3, 2.7, 23.1
//! expect: run
//! stdout: 5
//! issue: 9250
//! notes: Harvested from the #9250 review (acc_user_ob_generic), the admitting twin of s17_6_9901. The
//!   user's generic class shadows the auto-imported OwningBuffer<T> (2.3), so `a.Length` is its own
//!   field: 4, then 5.
module Conf;

public class OwningBuffer<T>
  public Length: int := 0;
end OwningBuffer;

public class Program
  public static method Main()
    var a := new OwningBuffer<int>();
    a.Length := 4;
    a.Length++;
    Console.WriteLine(a.Length);
  end Main;
end Program;
