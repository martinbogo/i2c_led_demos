import time

from arduino.app_utils import App, Bridge

original_notify = Bridge.notify
def notify_str(*args):
    return original_notify(*[str(a) if isinstance(a, int) else a for a in args])
Bridge.notify = notify_str

START_MONOTONIC = time.monotonic()
BASE_STEPS = 5500 + (int(time.time()) % 1000)


def current_watch_state() -> tuple[int, int, int]:
    now = time.localtime()
    day_seconds = now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec

    elapsed = time.monotonic() - START_MONOTONIC
    steps = min(14000, BASE_STEPS + int(elapsed * 2.8))
    battery = max(5, 87 - int(elapsed * 0.005))
    return day_seconds, steps, battery


def watch_sync() -> int:
    try:
        day_seconds, steps, battery = current_watch_state()
        Bridge.notify("watch_state", day_seconds, steps, battery)
        return 1
    except Exception as exc:
        print(f"watch sync failed: {exc}", flush=True)
        return 0


Bridge.provide("watch_sync", watch_sync)

App.run()
