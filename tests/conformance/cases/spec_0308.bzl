var outcome := client.Fetch(url);       // owned move-only local
case outcome of                          // borrowing match; outcome unchanged
  Failed(var s, _) when s >= 500 => ScheduleRetry(),
  _                              => ()
end;
// outcome is still owned here; discharge or auto-drop per §28.3.
