//! case: s11_2_0003
//! doc: spec
//! section: 11.2
//! title: an implementation may widen an interface parameter from T to T?
//! rule: An implementation may declare a parameter as T? where the interface
//!   requires T (the implementation accepts more inputs than the interface
//!   promises to pass).
//! cites: 11.2
//! expect: accept
module Conf;

public interface IHandler
  method Handle(value: string);
end IHandler;

public class LenientHandler: IHandler
  public method Handle(value: string?)
    Console.WriteLine(value ?? "none");
  end Handle;
end LenientHandler;
