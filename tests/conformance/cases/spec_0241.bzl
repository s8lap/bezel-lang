do
  using var t := semaphore.AcquireAsync();
  // No await; nothing transferred. If t completes before we leave the block,
  // its produced token is disposed (the semaphore is released).
end;
