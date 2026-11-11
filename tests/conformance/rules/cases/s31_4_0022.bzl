//! case: s31_4_0022
//! doc: spec
//! section: 31.4
//! title: a class under InvalidOperationException without the marker is admitted
//! rule: Every other `Exception`-derived class may list it, whatever its base:
//!   a class deriving from `InvalidOperationException`, from `FormatException`
//!   or from a class of the program's own is a failure where its author says
//!   so, and an arm naming its base still takes it.
//! cites: 31.4
//! expect: accept
//! issue: 8416
module Conf;

public class OrderError(message: string): InvalidOperationException(message)
end OrderError;
