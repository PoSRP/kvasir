# monitor

Real-time monitor and HDF5 logger for one or more kvasir boards. Driven by a
YAML config that maps each board (by USB serial) to the channels it should
capture.

Run it:

```
./tools/monitor/run                              # uses example.yaml
./tools/monitor/run mycfg.yaml
./tools/monitor/run mycfg.yaml --output run.h5
./tools/monitor/run mycfg.yaml --no-gui --output run.h5
```

`run` bootstraps a local venv from `requirements.txt` and invokes
`python -m tools.monitor` under it.
