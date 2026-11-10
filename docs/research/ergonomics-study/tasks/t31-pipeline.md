# t31-pipeline

*Area:* async & concurrency

A three-stage pipeline over channels: a producer sends the words `alpha beta gamma`, a transformer uppercases each and forwards it, a consumer prints `ALPHA`, `BETA`, `GAMMA` in order then `done 3`. Stages run concurrently; closing propagates end-of-stream.
