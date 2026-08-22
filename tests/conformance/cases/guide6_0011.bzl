public pure method Accepts(state: int): bool
  return state >= 0 and state < _stateCount;
end Accepts;

public method Transition(state: int)
  requires(Accepts(state))

  _current := state;
end Transition;
