//! case: s32_5_0009
//! doc: spec
//! section: 32.5
//! title: a record that is not C-compatible is rejected as a parameter type
//! rule: Not allowed: ... records other than the §32.13 forms.
//! cites: 32.5, 32.13
//! expect: reject
//! diagnostic: c-compatible
module Conf;

public record Config(Name: string, Retries: int);

extern "libexample"
  method Apply(cfg: Config): int;
end;
