import time

from arduino.app_utils import App, Bridge


START_MONOTONIC = time.monotonic()
BASE_STEPS = 5500 + (int(time.time()) % 1000)


def current_watch_state() -> tuple[int, int, int]:
    now = time.localtime()
    day_seconds = now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec

    elapsed = time.monotonic() - START_MONOTONIC
    steps = min(14000, BASE_STEPS + int(elapsed * 2.8))
    battery = max(5, 87 - int(elapsed * 0.005))
    return day_seconds, steps, battery


def loop() -> None:
    day_seconds, steps, battery = current_watch_state()
    try:
        Bridge.notify("watch_sync", day_seconds, steps, battery)
    except Exception as exc:
        print(f"watch sync failed: {exc}", flush=True)
    time.sleep(0.5)


App.run(user_loop=loop)
