//! case: s22_12_0016
//! doc: spec
//! section: 22.12
//! title: a static owned readonly SharedEventSource field is the admitted warning-free publication point
//! rule: A process-global publication point is an ordinary static field
//!   of type `SharedEventSource<T>` — a form-(i) member, admitted
//!   warning-free in the recommended shape
//!   `private static owned readonly _e: SharedEventSource<T> := new();`.
//! cites: 22.12
//! expect: accept
module Conf;

public static class Bus
  private static owned readonly _e: SharedEventSource<int> := new();
end Bus;
