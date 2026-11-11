//! case: s31_4_0024
//! doc: spec
//! section: 31.4
//! title: IFailure is admitted on a class whose user base derives from Exception
//! rule: Every other `Exception`-derived class may list it, whatever its base:
//!   a class deriving from `InvalidOperationException`, from `FormatException`
//!   or from a class of the program's own is a failure where its author says
//!   so, and an arm naming its base still takes it.
//! cites: 31.4
//! expect: accept
//! issue: 8416
module Conf;

public class BadField(message: string): Exception(message)
end BadField;

public class BadPort(message: string): BadField(message), IFailure
end BadPort;
