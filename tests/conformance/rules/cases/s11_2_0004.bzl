//! case: s11_2_0004
//! doc: spec
//! section: 11.2
//! title: an implementation may not narrow an interface parameter from T? to T
//! rule: The reverse -- interface T?, implementation T -- is rejected, because
//!   the implementation would refuse a null the interface promised to accept.
//! cites: 11.2
//! expect: reject
//! diagnostic: implement
module Conf;

public interface IHandler
  method Handle(value: string?);
end IHandler;

public class StrictHandler: IHandler
  public method Handle(value: string)
    Console.WriteLine(value);
  end Handle;
end StrictHandler;
