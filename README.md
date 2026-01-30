# seu-injection-lab
## Generate logs and plots
```
docker build -t seu .
docker run --rm -v "$PWD/logs:/work/logs" -v "$PWD/plots:/work/plots" seu
```

## Third-party code
Folder `FreeRTOS/` is third-party code (the FreeRTOS kernel) and is not authored by me. It is included as an external library dependency for this project.
