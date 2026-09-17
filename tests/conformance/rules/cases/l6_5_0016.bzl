//! case: l6_5_0016
//! doc: stdlib
//! section: 6.5
//! title: SharedEventSource does not implement IEvent
//! rule: SharedEventSource<T> deliberately does not implement IEvent<T> — an
//!   upcast would launder the capture-free requirement through
//!   IEvent<T>.Subscribe's capture-admitting `scoped` signature.
//! cites: 6.5
//! expect: reject
//! diagnostic: IEvent
module Conf;

public class Program
  public static method Demo()
    using var src := new SharedEventSource<int>();
    var e: IEvent<int> := src;
    using var sub := e.Subscribe(lambda(n) => Console.WriteLine(n));
  end Demo;
end Program;
