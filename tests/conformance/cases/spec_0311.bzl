case move outcome of
  Fetched(var r) when r.IsStale() => do _ := r; RefetchLater(); end,
  Fetched(var r)                  => do using var resp := move r; Render(resp); end,
  _                               => ()
end;
