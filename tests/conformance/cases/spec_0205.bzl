// Wrapper's constructor takes `owned inner: Buffer`; CloneBuffer(): owned Buffer.
public method Wrap(s: Buffer): owned Wrapper
  using var copy := s.CloneBuffer();   // copy is owned + scope-bound
  return new Wrapper(move copy);       // ERROR: cannot move out of a `using var`
end Wrap;

public method Wrap2(owned s: Buffer): owned Wrapper
  return new Wrapper(move s);          // OK: s is moved into Wrapper's constructor
                                       // s is now in moved-from state
end Wrap2;
