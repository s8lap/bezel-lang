public choice Result<T, E>
  Ok(owned value: T),
  Error(owned error: E);

  public method IsOk(): bool => (case this of
    Ok(_)    => true,
    Error(_) => false);

  public method IsError(): bool => not IsOk();

  // Compiler-provided (spec §35.2): admissible only where the receiver AND the
  // member's result instantiation are value-like; a call at (or producing) a
  // borrow-bearing or owning instantiation is rejected at the use site (spec
  // §3.6 rule 2), the diagnostic naming the classification and the deciding
  // payload.
  public method Map<U>(f: (T) -> U): Result<U, E>;
  public method MapError<F>(f: (E) -> F): Result<T, F>;
  public method AndThen<U>(f: (T) -> Result<U, E>): Result<U, E>;
  public method UnwrapOr(fallback: T): T;
end Result;
