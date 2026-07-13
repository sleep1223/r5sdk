```shell
python .\scripts\r5_release.py build --config Release
python .\scripts\r5_release.py build --config Release --no-generate
python .\scripts\r5_release.py pull --artifact game --version latest
python .\scripts\r5_release.py pull --artifact symbols_debug --version 2026063002 --config Release
python .\scripts\r5_release.py upload --server server-1
python .\scripts\r5_release.py upload --server server-1 --server server-2
python .\scripts\r5_release.py all --config Release --all-servers --pull-symbols

```

```shell
python .\scripts\r5_release.py build --config Debug
python .\scripts\r5_release.py build --config Debug --no-generate


python .\scripts\r5_release.py upload --server quanzhou
python .\scripts\r5_release.py upload --all-servers


python .\scripts\r5_release.py all --config Debug --all-servers --no-generate


python .\scripts\r5_release.py pull --artifact game --version latest
python .\scripts\r5_release.py pull --artifact symbols_debug --version latest
```
