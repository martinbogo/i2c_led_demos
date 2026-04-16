import re

with open('sketch/sketch.ino', 'r') as f:
    sketch = f.read()

# dashBegin
sketch = re.sub(
    r'void dashBegin\(uint32_t (uptimeMinutes), uint32_t (procs), int32_t (tempC), int32_t (fanValue), uint32_t (fanIsRPM)\) \{',
    r'void dashBegin(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  int32_t \3 = \3_str.toInt();\n  int32_t \4 = \4_str.toInt();\n  uint32_t \5 = \5_str.toInt();',
    sketch
)

# dashSystem
sketch = re.sub(
    r'void dashSystem\(uint32_t (armFreqMHz), uint32_t (armMaxMHz), uint32_t (totalCpu10), uint32_t (iowait10), uint32_t (load1x10)\) \{',
    r'void dashSystem(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  uint32_t \3 = \3_str.toInt();\n  uint32_t \4 = \4_str.toInt();\n  uint32_t \5 = \5_str.toInt();',
    sketch
)

# dashCores
sketch = re.sub(
    r'void dashCores\(uint32_t (coreCount), uint32_t (cpu0), uint32_t (cpu1), uint32_t (cpu2), uint32_t (cpu3)\) \{',
    r'void dashCores(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  uint32_t \3 = \3_str.toInt();\n  uint32_t \4 = \4_str.toInt();\n  uint32_t \5 = \5_str.toInt();',
    sketch
)

# dashCoreFreqs
sketch = re.sub(
    r'void dashCoreFreqs\(uint32_t (freq0), uint32_t (freq1), uint32_t (freq2), uint32_t (freq3)\) \{',
    r'void dashCoreFreqs(String \1_str, String \2_str, String \3_str, String \4_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  uint32_t \3 = \3_str.toInt();\n  uint32_t \4 = \4_str.toInt();',
    sketch
)

# dashMemory
sketch = re.sub(
    r'void dashMemory\(uint32_t (memUsed10), uint32_t (memAvailMB), uint32_t (memCachedMB),\s*uint32_t (swapUsedMB), uint32_t (zramUsedMB), uint32_t (memPsi100)\) \{',
    r'void dashMemory(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str, String \6_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  uint32_t \3 = \3_str.toInt();\n  uint32_t \4 = \4_str.toInt();\n  uint32_t \5 = \5_str.toInt();\n  uint32_t \6 = \6_str.toInt();',
    sketch
)

# dashStorage
sketch = re.sub(
    r'void dashStorage\(uint32_t (rootTagCode), uint32_t (diskUsed10), uint32_t (diskReadKBps),\s*uint32_t (diskWriteKBps), int32_t (nvmeTempC), uint32_t (throttledMask), uint32_t (failedUnits)\) \{',
    r'void dashStorage(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str, String \6_str, String \7_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  uint32_t \3 = \3_str.toInt();\n  uint32_t \4 = \4_str.toInt();\n  int32_t \5 = \5_str.toInt();\n  uint32_t \6 = \6_str.toInt();\n  uint32_t \7 = \7_str.toInt();',
    sketch
)

# dashNetwork
sketch = re.sub(
    r'void dashNetwork\(uint32_t (ethUp), uint32_t (wlanUp), int32_t (ethSpeedMbps), int32_t (wifiRssiDbm),\s*uint32_t (netRxKBps), uint32_t (netTxKBps), int32_t (gatewayLatency10)\) \{',
    r'void dashNetwork(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str, String \6_str, String \7_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  int32_t \3 = \3_str.toInt();\n  int32_t \4 = \4_str.toInt();\n  uint32_t \5 = \5_str.toInt();\n  uint32_t \6 = \6_str.toInt();\n  int32_t \7 = \7_str.toInt();',
    sketch
)

with open('sketch/sketch.ino', 'w') as f:
    f.write(sketch)

print("Patch applied to sketch")

with open('python/main.py', 'r') as f:
    main_py = f.read()

# Replace send() helper to convert to strings
main_py = main_py.replace('def notify(*args):', 'def notify(*args):\n    args = [str(a) if isinstance(a, int) else a for a in args]')
with open('python/main.py', 'w') as f:
    f.write(main_py)
print("Patch applied to main.py")
