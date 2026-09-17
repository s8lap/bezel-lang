//! case: s12_10_0004
//! doc: spec
//! section: 12.10
//! title: a two-interface extension cycle is rejected
//! rule: The relation followed is the union of base-class edges and
//!   extended-interface edges; a cycle is any sequence of types where each
//!   lists the next as a base type and the sequence returns to its start.
//! cites: 12.10, C.1
//! expect: reject
//! diagnostic: cycle
module Conf;

public interface IA: IB
  method PingA(): int;
end IA;

public interface IB: IA
  method PingB(): int;
end IB;
