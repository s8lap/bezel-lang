//! case: l4_14_0001
//! doc: stdlib
//! section: 4.14
//! title: a Span<T> may not outlive the OwningBuffer<T> source it borrows
//! rule: A borrow-bearing record value may not outlive the source of any
//!   borrow it contains; a Span<T>'s Buffer field carries a borrow of the
//!   OwningBuffer<T> it views, bounded by the source's lifetime.
//! cites: 4.14, 9.7
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Program
  public method Demo(flag: bool)
    var s: Span<int>;
    if flag then
      using var buf := new OwningBuffer<int>(3);
      s := buf.AsSpan();
    end;
  end Demo;
end Program;
