public choice AcquireOutcome
  Ready(conns: [4 owned]Connection?),          // move-only value payload — owning (§35.4)
  Empty;
end AcquireOutcome;

case pool.Acquire() of                         // owning match over the temporary
  Ready(var b) => Dispatch(b),                 // b transfers at selection (§15.3 local);
                                               //   the call consumes it by value-move
  Empty        => ()
end;
// A Ready(_) arm, or an unmatched Ready under a `_` arm, would instead dispose
// the array's slots at construct completion — trace (b) below, unchanged.
