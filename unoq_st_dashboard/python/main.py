import os
import time
from dataclasses import dataclass, field
from pathlib import Path

from arduino.app_utils import App, Bridge

original_notify = Bridge.notify
def notify_str(*args):
    return original_notify(*[str(a) if isinstance(a, int) else a for a in args])
Bridge.notify = notify_str


MAX_CORES = 4
THERMAL_LIMIT_C = 85
ROOT_UNKNOWN = 0
ROOT_NVME = 1
ROOT_SD = 2
ROOT_USB = 3


@dataclass
class CpuSnap:
    user: int = 0
    nice: int = 0
    system: int = 0
    idle: int = 0
    iowait: int = 0
    irq: int = 0
    softirq: int = 0
    steal: int = 0

    @classmethod
    def from_fields(cls, fields: list[str]) -> "CpuSnap | None":
        if len(fields) < 8:
            return None
        return cls(*(int(value) for value in fields[:8]))


@dataclass
class Snapshot:
    uptime_minutes: int = 0
    procs: int = 0
    temp_c: int = 0
    fan_value: int = -1
    fan_is_rpm: bool = False
    arm_freq_mhz: int = 0
    arm_max_mhz: int = 0
    total_cpu10: int = 0
    iowait10: int = 0
    load1x10: int = 0
    core_count: int = 0
    per_core10: list[int] = field(default_factory=lambda: [0] * MAX_CORES)
    core_freq_mhz: list[int] = field(default_factory=lambda: [0] * MAX_CORES)
    mem_used10: int = 0
    mem_avail_mb: int = 0
    mem_cached_mb: int = 0
    swap_used_mb: int = 0
    zram_used_mb: int = 0
    mem_psi100: int = 0
    root_tag_code: int = ROOT_UNKNOWN
    disk_used10: int = 0
    disk_read_kbps: int = 0
    disk_write_kbps: int = 0
    nvme_temp_c: int = -1
    throttled_mask: int = 0
    failed_units: int = 0
    eth_up: bool = False
    wlan_up: bool = False
    eth_speed_mbps: int = -1
    wifi_rssi_dbm: int = -1000
    net_rx_kbps: int = 0
    net_tx_kbps: int = 0
    gateway_latency10: int = -1


prev_total_cpu: CpuSnap | None = None
prev_core_cpu: list[CpuSnap | None] = [None] * MAX_CORES
prev_net: tuple[int, int, float] | None = None
prev_disk: tuple[int, int, float] | None = None


def read_text(path: str) -> str | None:
    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except OSError:
        return None


def read_int(path: str) -> int | None:
    text = read_text(path)
    if text is None:
        return None
    try:
        return int(text)
    except ValueError:
        return None


def clamp(value: int, lo: int, hi: int) -> int:
    return max(lo, min(value, hi))


def pct10(numerator: float) -> int:
    return clamp(int(round(numerator * 10.0)), 0, 1000)


def best_temp_c() -> int:
    best = 0
    for zone in range(64):
        temp = read_int(f"/sys/class/thermal/thermal_zone{zone}/temp")
        if temp is None:
            continue
        if 1000 <= temp <= 200000:
            best = max(best, temp // 1000)
    return best


def fan_value() -> tuple[int, bool]:
    for hwmon in range(32):
        value = read_int(f"/sys/class/hwmon/hwmon{hwmon}/fan1_input")
        if value is not None:
            return value, value > 255
    for idx in range(16):
        type_name = read_text(f"/sys/class/thermal/cooling_device{idx}/type")
        if not type_name or ("fan" not in type_name and "pwm" not in type_name):
            continue
        value = read_int(f"/sys/class/thermal/cooling_device{idx}/cur_state")
        if value is not None:
            return value, False
    return -1, False


def read_cpu_freq_mhz(cpu: int) -> int:
    for path in (
        f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq",
        f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/cpuinfo_cur_freq",
    ):
        value = read_int(path)
        if value is not None:
            return value // 1000
    return 0


def read_cpu_max_mhz() -> int:
    for path in (
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
    ):
        value = read_int(path)
        if value is not None:
            return value // 1000
    return 0


def read_cpu_snaps() -> tuple[CpuSnap, list[CpuSnap], int]:
    total = CpuSnap()
    cores: list[CpuSnap] = [CpuSnap() for _ in range(MAX_CORES)]
    core_count = 0
    try:
        with open("/proc/stat", "r", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("cpu "):
                    snap = CpuSnap.from_fields(line.split()[1:])
                    if snap is not None:
                        total = snap
                elif line.startswith("cpu") and len(line) > 3 and line[3].isdigit():
                    idx = int(line[3: line.find(" ")])
                    if 0 <= idx < MAX_CORES:
                        snap = CpuSnap.from_fields(line.split()[1:])
                        if snap is not None:
                            cores[idx] = snap
                            core_count = max(core_count, idx + 1)
    except OSError:
        pass
    return total, cores, core_count


def calc_cpu_pct(prev: CpuSnap, curr: CpuSnap) -> float:
    idle_prev = prev.idle + prev.iowait
    idle_curr = curr.idle + curr.iowait
    total_prev = prev.user + prev.nice + prev.system + prev.idle + prev.iowait + prev.irq + prev.softirq + prev.steal
    total_curr = curr.user + curr.nice + curr.system + curr.idle + curr.iowait + curr.irq + curr.softirq + curr.steal
    total_delta = total_curr - total_prev
    idle_delta = idle_curr - idle_prev
    if total_delta <= 0:
        return 0.0
    return 100.0 * (total_delta - idle_delta) / total_delta


def calc_iowait_pct(prev: CpuSnap, curr: CpuSnap) -> float:
    total_prev = prev.user + prev.nice + prev.system + prev.idle + prev.iowait + prev.irq + prev.softirq + prev.steal
    total_curr = curr.user + curr.nice + curr.system + curr.idle + curr.iowait + curr.irq + curr.softirq + curr.steal
    total_delta = total_curr - total_prev
    if total_delta <= 0:
        return 0.0
    return 100.0 * (curr.iowait - prev.iowait) / total_delta


def read_meminfo() -> dict[str, int]:
    values = {
        "MemTotal": 0,
        "MemAvailable": 0,
        "Cached": 0,
        "SwapTotal": 0,
        "SwapFree": 0,
    }
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as fh:
            for line in fh:
                key, _, rest = line.partition(":")
                if key in values:
                    try:
                        values[key] = int(rest.strip().split()[0])
                    except (IndexError, ValueError):
                        values[key] = 0
    except OSError:
        pass
    return values


def read_psi100(path: str) -> int:
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("some "):
                    for token in line.split():
                        if token.startswith("avg10="):
                            return max(0, int(round(float(token.split("=", 1)[1]) * 100.0)))
    except (OSError, ValueError):
        pass
    return 0


def read_zram_used_mb() -> int:
    total_kb = 0
    for idx in range(8):
        path = f"/sys/block/zram{idx}/mm_stat"
        text = read_text(path)
        if not text:
            continue
        parts = text.split()
        if len(parts) >= 3:
            try:
                total_kb += int(parts[2]) // 1024
            except ValueError:
                continue
    return total_kb // 1024


def root_base_device(device: str) -> str:
    base = device.rsplit("/", 1)[-1]
    if base.startswith("nvme") or base.startswith("mmcblk"):
        if "p" in base and base.rsplit("p", 1)[-1].isdigit():
            base = base.rsplit("p", 1)[0]
    else:
        while base and base[-1].isdigit():
            base = base[:-1]
    return base


def detect_root_device() -> tuple[str, int]:
    try:
        with open("/proc/mounts", "r", encoding="utf-8") as fh:
            for line in fh:
                parts = line.split()
                if len(parts) >= 2 and parts[1] == "/":
                    base = root_base_device(parts[0])
                    if "nvme" in base:
                        return base, ROOT_NVME
                    if "mmcblk" in base:
                        return base, ROOT_SD
                    if base.startswith("sd"):
                        return base, ROOT_USB
                    return base, ROOT_UNKNOWN
    except OSError:
        pass
    return "", ROOT_UNKNOWN


def read_disk_totals(root_dev: str) -> tuple[int, int]:
    if not root_dev:
        return 0, 0
    text = read_text(f"/sys/block/{root_dev}/stat")
    if not text:
        return 0, 0
    parts = text.split()
    if len(parts) < 7:
        return 0, 0
    try:
        return int(parts[2]) * 512, int(parts[6]) * 512
    except ValueError:
        return 0, 0


def disk_used10() -> int:
    st = os.statvfs("/")
    total = st.f_blocks * st.f_frsize
    used = total - (st.f_bfree * st.f_frsize)
    if total <= 0:
        return 0
    return pct10((used / total) * 100.0)


def read_nvme_temp_c() -> int:
    for hwmon in range(32):
        name = read_text(f"/sys/class/hwmon/hwmon{hwmon}/name")
        if not name or "nvme" not in name:
            continue
        temp = read_int(f"/sys/class/hwmon/hwmon{hwmon}/temp1_input")
        if temp is not None:
            return temp // 1000
    return -1


def net_totals() -> tuple[int, int]:
    rx_total = 0
    tx_total = 0
    try:
        with open("/proc/net/dev", "r", encoding="utf-8") as fh:
            for line in fh:
                if ":" not in line:
                    continue
                name, data = line.split(":", 1)
                if name.strip() == "lo":
                    continue
                fields = data.split()
                if len(fields) >= 9:
                    rx_total += int(fields[0])
                    tx_total += int(fields[8])
    except (OSError, ValueError):
        pass
    return rx_total, tx_total


def find_interfaces() -> tuple[str, str]:
    eth = ""
    wlan = ""
    try:
        for name in os.listdir("/sys/class/net"):
            if not eth and (name.startswith("eth") or name.startswith("en")):
                eth = name
            if not wlan and (name.startswith("wlan") or name.startswith("wl")):
                wlan = name
    except OSError:
        pass
    return eth, wlan


def read_iface_carrier(ifname: str) -> bool:
    if not ifname:
        return False
    value = read_int(f"/sys/class/net/{ifname}/carrier")
    return value == 1


def read_eth_speed_mbps(ifname: str) -> int:
    if not ifname:
        return -1
    value = read_int(f"/sys/class/net/{ifname}/speed")
    return value if value is not None else -1


def read_wifi_rssi_dbm(ifname: str) -> int:
    if not ifname:
        return -1000
    try:
        with open("/proc/net/wireless", "r", encoding="utf-8") as fh:
            for line in fh:
                if ":" not in line:
                    continue
                name, rest = line.split(":", 1)
                if name.strip() != ifname:
                    continue
                parts = rest.split()
                if len(parts) >= 3:
                    return int(round(float(parts[2])))
    except (OSError, ValueError):
        pass
    return -1000


def uptime_minutes_and_procs() -> tuple[int, int]:
    uptime_minutes = 0
    procs = 0
    try:
        uptime_text = read_text("/proc/uptime")
        if uptime_text:
            uptime_minutes = int(float(uptime_text.split()[0]) // 60)
    except ValueError:
        uptime_minutes = 0

    try:
        with open("/proc/loadavg", "r", encoding="utf-8") as fh:
            fields = fh.read().split()
            if len(fields) >= 4 and "/" in fields[3]:
                procs = int(fields[3].split("/", 1)[1])
    except (OSError, ValueError):
        procs = 0
    return uptime_minutes, procs


def collect_snapshot() -> Snapshot:
    global prev_total_cpu, prev_core_cpu, prev_net, prev_disk

    snapshot = Snapshot()
    now_monotonic = time.monotonic()

    snapshot.temp_c = best_temp_c()
    snapshot.fan_value, snapshot.fan_is_rpm = fan_value()
    snapshot.arm_freq_mhz = read_cpu_freq_mhz(0)
    snapshot.arm_max_mhz = read_cpu_max_mhz()
    snapshot.load1x10 = pct10(os.getloadavg()[0]) if hasattr(os, "getloadavg") else 0

    total_cpu, cores, core_count = read_cpu_snaps()
    snapshot.core_count = core_count
    for idx in range(core_count):
        snapshot.core_freq_mhz[idx] = read_cpu_freq_mhz(idx)
    if prev_total_cpu is not None:
        snapshot.total_cpu10 = pct10(calc_cpu_pct(prev_total_cpu, total_cpu))
        snapshot.iowait10 = pct10(calc_iowait_pct(prev_total_cpu, total_cpu))
        for idx in range(core_count):
            prev_core = prev_core_cpu[idx]
            if prev_core is not None:
                snapshot.per_core10[idx] = pct10(calc_cpu_pct(prev_core, cores[idx]))
    prev_total_cpu = total_cpu
    prev_core_cpu = cores

    meminfo = read_meminfo()
    total_kb = meminfo["MemTotal"]
    avail_kb = meminfo["MemAvailable"]
    cached_kb = meminfo["Cached"]
    swap_total_kb = meminfo["SwapTotal"]
    swap_free_kb = meminfo["SwapFree"]
    used_kb = max(0, total_kb - avail_kb)
    snapshot.mem_used10 = pct10((used_kb / total_kb) * 100.0) if total_kb else 0
    snapshot.mem_avail_mb = avail_kb // 1024
    snapshot.mem_cached_mb = cached_kb // 1024
    snapshot.swap_used_mb = max(0, swap_total_kb - swap_free_kb) // 1024
    snapshot.zram_used_mb = read_zram_used_mb()
    snapshot.mem_psi100 = read_psi100("/proc/pressure/memory")

    root_dev, root_tag_code = detect_root_device()
    snapshot.root_tag_code = root_tag_code
    snapshot.disk_used10 = disk_used10()
    snapshot.nvme_temp_c = read_nvme_temp_c()
    disk_read_b, disk_write_b = read_disk_totals(root_dev)
    if prev_disk is not None:
        prev_read_b, prev_write_b, prev_time = prev_disk
        dt = now_monotonic - prev_time
        if dt > 0:
            snapshot.disk_read_kbps = int(max(0.0, (disk_read_b - prev_read_b) / dt / 1024.0))
            snapshot.disk_write_kbps = int(max(0.0, (disk_write_b - prev_write_b) / dt / 1024.0))
    prev_disk = (disk_read_b, disk_write_b, now_monotonic)

    rx_b, tx_b = net_totals()
    if prev_net is not None:
        prev_rx_b, prev_tx_b, prev_time = prev_net
        dt = now_monotonic - prev_time
        if dt > 0:
            snapshot.net_rx_kbps = int(max(0.0, (rx_b - prev_rx_b) / dt / 1024.0))
            snapshot.net_tx_kbps = int(max(0.0, (tx_b - prev_tx_b) / dt / 1024.0))
    prev_net = (rx_b, tx_b, now_monotonic)

    eth_if, wlan_if = find_interfaces()
    snapshot.eth_up = read_iface_carrier(eth_if)
    snapshot.wlan_up = read_iface_carrier(wlan_if)
    snapshot.eth_speed_mbps = read_eth_speed_mbps(eth_if)
    snapshot.wifi_rssi_dbm = read_wifi_rssi_dbm(wlan_if)

    snapshot.uptime_minutes, snapshot.procs = uptime_minutes_and_procs()
    return snapshot


def send_snapshot(snapshot: Snapshot) -> None:
    Bridge.notify("dash_test", "10", "hello")
    Bridge.notify(
        "dash_begin",
        snapshot.uptime_minutes,
        snapshot.procs,
        snapshot.temp_c,
        snapshot.fan_value,
        int(snapshot.fan_is_rpm),
    )
    time.sleep(0.02)
    Bridge.notify(
        "dash_system",
        snapshot.arm_freq_mhz,
        snapshot.arm_max_mhz,
        snapshot.total_cpu10,
        snapshot.iowait10,
        snapshot.load1x10,
    )
    time.sleep(0.02)
    Bridge.notify(
        "dash_cores",
        snapshot.core_count,
        snapshot.per_core10[0],
        snapshot.per_core10[1],
        snapshot.per_core10[2],
        snapshot.per_core10[3],
    )
    time.sleep(0.02)
    Bridge.notify(
        "dash_core_freqs",
        snapshot.core_freq_mhz[0],
        snapshot.core_freq_mhz[1],
        snapshot.core_freq_mhz[2],
        snapshot.core_freq_mhz[3],
    )
    time.sleep(0.02)
    Bridge.notify(
        "dash_memory",
        snapshot.mem_used10,
        snapshot.mem_avail_mb,
        snapshot.mem_cached_mb,
        snapshot.swap_used_mb,
        snapshot.zram_used_mb,
        snapshot.mem_psi100,
    )
    time.sleep(0.02)
    Bridge.notify(
        "dash_storage",
        snapshot.root_tag_code,
        snapshot.disk_used10,
        snapshot.disk_read_kbps,
        snapshot.disk_write_kbps,
        snapshot.nvme_temp_c,
        snapshot.throttled_mask,
        snapshot.failed_units,
    )
    time.sleep(0.02)
    Bridge.notify(
        "dash_network",
        int(snapshot.eth_up),
        int(snapshot.wlan_up),
        snapshot.eth_speed_mbps,
        snapshot.wifi_rssi_dbm,
        snapshot.net_rx_kbps,
        snapshot.net_tx_kbps,
        snapshot.gateway_latency10,
    )
    time.sleep(0.02)
    Bridge.notify("dash_commit")


import time
import threading

def _delayed_sync():
    time.sleep(0.2)
    try:
        send_snapshot(collect_snapshot())
    except Exception as exc:
        print(f"dash_sync delayed ERROR: {exc}", flush=True)

def dash_sync() -> int:
    t0 = time.time()
    try:
        threading.Thread(target=_delayed_sync, daemon=True).start()
        t1 = time.time()
        print(f"dash_sync returning int took {t1-t0:.3f}s", flush=True)
        return 1
    except Exception as exc:
        print(f"dashboard sync failed: {exc}", flush=True)
        return 0


Bridge.provide("dash_sync", dash_sync)

App.run()
