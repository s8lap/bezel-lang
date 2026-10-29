//! case: s22_5_0030
//! doc: spec
//! section: 22.5
//! title: BinaryDeserializer with only ignoreUnknownFields named leaves input unbound
//! rule: Each supplied argument binds exactly one parameter and each parameter is bound by at most one
//!   supplied argument; every parameter that declares no default must be bound (spec 22.5). `BinaryDeserializer(input, ignoreUnknownFields := false)` declares no default for `input`.
//! cites: 22.5, 22.6
//! expect: reject
//! diagnostic: not supplied
module Conf;

import System.Serialization;
import System.Threading;

public class Program
  public static method Main()
    using var w := new BinarySerializer();
    w.WriteInt(1);
    using var bytes := w.ToBytes();
    using var x := new BinaryDeserializer(ignoreUnknownFields := true);
  end Main;
end Program;
