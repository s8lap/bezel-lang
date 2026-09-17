//! case: l3_3_0001
//! doc: stdlib
//! section: 3.3
//! title: ReadOnlyStringSpan is not borrow-bearing and may sit in a field
//! rule: ReadOnlyStringSpan is a deliberate exception among the spans — its
//!   only reference field is a string, so it may sit in fields and cross
//!   await.
//! cites: 3.3, 9.7
//! expect: run
//! stdout: hello
module Conf;

public class Holder
  private _span: ReadOnlyStringSpan;

  public constructor(s: ReadOnlyStringSpan)
    _span := s;
  end constructor;

  public method Text(): string => _span.ToString();
end Holder;

public class Program
  public static method Main()
    var s := "hello world";
    using var h := new Holder(s.AsSpan(0, 5));
    Console.WriteLine(h.Text());
  end Main;
end Program;
