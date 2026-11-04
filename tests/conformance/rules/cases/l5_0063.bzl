//! case: l5_0063
//! doc: stdlib
//! section: 5
//! title: Min and Max with a Guid selector work on LinkedList and Set receivers
//! rule: public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 2.6, 5
//! expect: run
//! output: LL 00000000-0000-0000-0000-000000000005 01000000-0000-0000-0000-000000000000
//!   SET 00000000-0000-0000-0000-000000000005 01000000-0000-0000-0000-000000000000
//! issue: 8048
//! notes: twin of l5_0060 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    using var ll := new LinkedList<Guid>();
    ll.Add(Guid.Parse("00000000-0000-0000-0001-000000000000"));
    ll.Add(Guid.Parse("01000000-0000-0000-0000-000000000000"));
    ll.Add(Guid.Parse("00000000-0000-0000-0000-000000000005"));
    Console.WriteLine("LL " + ll.Min(lambda(x: Guid) => x).ToString() + " " + ll.Max(lambda(x: Guid) => x).ToString());
    using var st := new Set<Guid>();
    st.Add(Guid.Parse("00000000-0000-0000-0001-000000000000"));
    st.Add(Guid.Parse("01000000-0000-0000-0000-000000000000"));
    st.Add(Guid.Parse("00000000-0000-0000-0000-000000000005"));
    Console.WriteLine("SET " + st.Min(lambda(x: Guid) => x).ToString() + " " + st.Max(lambda(x: Guid) => x).ToString());
  end Main;
end Program;
