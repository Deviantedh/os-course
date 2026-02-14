#!/bin/bash
set -e

if [[ "$1" == "clear" ]]; then
  echo "Удаляю исполняемые файлы..."
  rm -f cpu-linreg proc-clone3 ema-traverse-graph
  echo "Готово!"
  exit 0
fi

gcc -std=gnu11 -O2 -Wall -Wextra -o cpu-linreg cpu-linreg.c
gcc -std=gnu11 -O2 -Wall -Wextra -o proc-clone3 proc-clone3.c
gcc -std=gnu11 -O2 -Wall -Wextra -o ema-traverse-graph ema-traverse-graph.c

echo "Сборка завершена успешно!"
