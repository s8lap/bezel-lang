//! case: s35_1_0001
//! doc: spec
//! section: 35.1
//! title: a choice declares payload-free, value-payload, and owned-payload cases together
//! rule: A choice declares zero or more positional payload fields per case; a case with
//!   no payload list is payload-free; the owned marker is admissible on a class-typed
//!   payload field.
//! cites: 35.1, 35.2
//! expect: accept
module Conf;

public class ResponseBody
  public constructor()
  end constructor;
end ResponseBody;

public choice FetchOutcome
  Fetched(owned body: ResponseBody),
  NotModified,
  Failed(status: int, message: string);

  public method IsSuccess(): bool => (case this of
    Fetched(_)   => true,
    NotModified  => true,
    Failed(_, _) => false);
end FetchOutcome;
