case client.Fetch(url) of
  Fetched(var r) => do
    using var resp := move r;    // r moved into resp; resp disposed at arm-body end
    Render(resp);
  end,
  NotModified          => RenderCached(),
  Failed(var s, var m) => ShowError(s, m)
end;
