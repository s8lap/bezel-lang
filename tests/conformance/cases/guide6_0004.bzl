if not registry.TryAdd(move session) then    // declined: an equal element is already
  _rejected := _rejected + 1;                //   present; session is consumed either way,
end;                                         //   and on this path also disposed (stdlib §4.10)
