# seu-injection-lab
## Generate logs and plots
```
docker build -t seu .
docker run --rm -v "$PWD/logs:/work/logs" seu
```
