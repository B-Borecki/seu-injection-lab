FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    make qemu-system-arm gcc-arm-none-eabi libnewlib-arm-none-eabi gdb-multiarch \
    python3 python3-numpy python3-matplotlib ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
COPY . /work

RUN mkdir -p logs

RUN cat > /usr/local/bin/run_all_logs.sh << 'SH' && chmod +x /usr/local/bin/run_all_logs.sh
#!/usr/bin/env bash

cd /work
mkdir -p logs

wait_for_end() {
  local logfile="$1"
  tail -n 0 -F "$logfile" | grep -m1 -F "[END]" >/dev/null
}

run_baseline() {
  echo "[docker] baseline (no SEU, no GDB)"
  make clean >/dev/null
  make all PROTECT_MODE=0 >/dev/null

  local log="logs/seu_none.log"
  ( make run > "$log" 2>&1 ) &
  local qemu_pid=$!

  wait_for_end "$log"

  if kill -0 "$qemu_pid" >/dev/null 2>&1; then
    kill "$qemu_pid" >/dev/null 2>&1 || true
    wait "$qemu_pid" >/dev/null 2>&1 || true
  fi

  echo "[docker] baseline done: $log"
}

run_with_seu() {
  local seu_mode="$1"
  local protect_mode="$2"
  local suffix="$3"

  echo "[docker] SEU_MODE=${seu_mode} PROTECT_MODE=${protect_mode} ${suffix}"

  make clean >/dev/null
  make all PROTECT_MODE="$protect_mode" >/dev/null

  local seu_log="logs/seu_mode${seu_mode}${suffix}.log"
  local gdb_log="logs/gdb_mode${seu_mode}${suffix}.log"

  ( make run-debug > "$seu_log" 2>&1 ) &
  local qemu_pid=$!

  sleep 0.2

  ( make gdb SEU_MODE="$seu_mode" > "$gdb_log" 2>&1 ) &
  local gdb_pid=$!

  wait_for_end "$seu_log"

  if kill -0 "$gdb_pid" >/dev/null 2>&1; then
    kill "$gdb_pid" >/dev/null 2>&1 || true
    wait "$gdb_pid" >/dev/null 2>&1 || true
  fi

  if kill -0 "$qemu_pid" >/dev/null 2>&1; then
    kill "$qemu_pid" >/dev/null 2>&1 || true
    wait "$qemu_pid" >/dev/null 2>&1 || true
  fi

  echo "[docker] done: $seu_log, $gdb_log"
}

run_baseline
run_with_seu 0 0 ""
run_with_seu 1 0 ""
run_with_seu 2 0 ""
run_with_seu 1 1 "_sefi"
run_with_seu 2 2 "_sefi"

echo "[docker] all done. Logs in /work/logs:"
ls -1 logs || true

echo "[docker] generating plots..."
python3 plots_generator.py

echo "[docker] all done. Plots in /work/plots:"
ls -1 plots || true
SH

CMD ["/usr/local/bin/run_all_logs.sh"]
