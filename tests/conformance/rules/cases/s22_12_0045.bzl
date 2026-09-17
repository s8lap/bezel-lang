//! case: s22_12_0045
//! doc: spec
//! section: 22.12
//! title: static readonly is the warning-free spelling of a process-global primitive
//! rule: `static readonly` of **immutable content** — value types of sendable-value
//!   category, `string`, enums — is unrestricted (shared-immutable); it is the
//!   warning-free spelling where the value never changes after initialization.
//! cites: 22.12
//! expect: quiet
module Conf;

public static class Policy
  private static readonly _retries: int := 3;

  public static method Budget(): int
    return Policy._retries;
  end Budget;
end Policy;
