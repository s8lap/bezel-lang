public class Stats
  private _served: int := 0;

  public async method Track(job: Task): Task
    await job;
    _served := _served + 1;    // no lock, no atomic: continuations of one
  end Track;                   //   domain never overlap (spec §30.13)
end Stats;
