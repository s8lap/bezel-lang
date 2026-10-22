//! case: l4_11_0007
//! doc: stdlib
//! section: 4.11
//! title: a Stack<int> iterates top-to-bottom, the order its elements pop in
//! rule: "All four types iterate (via `IReadOnlyCollection<T>`) in take order — a stack top-to-bottom (pop order), a queue front-to-back (dequeue order); the owning forms iterate identically, yielding borrows."
//! cites: 4.11
//! expect: run
//! output: 3
//!   2
//!   1
module Conf;

public class Program
  public static method Main()
    using var st := new Stack<int>();
    st.Push(1);
    st.Push(2);
    st.Push(3);
    for var x in st do
      Console.WriteLine(x);
    end;
  end Main;
end Program;
