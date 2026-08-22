case client.Fetch(url) of                            // owning match (§35.5 trigger 1)
  Fetched(move response = var r)                     // marked slot: r is owned Response
    => RenderOwned(move r),                          //   in the arm, per §35.5's arm rules
  NotModified          => RenderCached(),
  Failed(var s, var m) => ShowError(s, m)
end;
