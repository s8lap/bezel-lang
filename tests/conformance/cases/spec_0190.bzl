public method Wrap(s: Stream): owned Wrapper
  using var copy := s.Clone();      // copy is owned + scope-bound
  return new Wrapper(copy);          // ERROR: copy is using-var, cannot move out
end Wrap;

public method Wrap2(owned s: Stream): owned Wrapper
  return new Wrapper(s);             // OK: s is moved into Wrapper's constructor
                                     // s is now in moved-from state
end Wrap2;
