#!/bin/bash
set -e
cd /src
run() {
  echo "===== $2 ====="
  cc -std=c11 -Wall -Wextra -Werror -Imain $1 -o /tmp/$2
  /tmp/$2
}
run 'tests/test_pomodoro_model.c main/pomodoro_model.c' test_model
run 'tests/test_pomodoro_date.c main/pomodoro_date.c' test_date
run 'tests/test_pomodoro_stats.c main/pomodoro_stats.c main/pomodoro_date.c' test_stats
run 'tests/test_pomodoro_blob.c main/pomodoro_blob.c main/pomodoro_date.c main/pomodoro_stats.c main/pomodoro_model.c' test_blob
run 'tests/test_ui_pixel_math.c main/ui_pixel_math.c' test_pixel
run 'tests/test_wifi_prov.c main/wifi_prov_util.c' test_wifi_prov
echo ALL_TEST_PASS
