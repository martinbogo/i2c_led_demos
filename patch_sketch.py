import re

with open('unoq_st_dashboard/sketch/sketch.ino', 'r') as f:
    sketch = f.read()

# dashBegin
sketch = re.sub(
    r'void dashBegin\(uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), int32_t ([A-Za-z0-9_]+), int32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+)\) \{',
    r'void dashBegin(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  int32_t \3 = \3_str.toInt();\n  int32_t \4 = \4_str.toInt();\n  uint32_t \5 = \5_str.toInt();',
    sketch
)

# dashSystem
sketch = re.sub(
    r'void dashSystem\(uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+)\) \{',
    r'void dashSystem(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  uint32_t \3 = \3_str.toInt();\n  uint32_t \4 = \4_str.toInt();\n  uint32_t \5 = \5_str.toInt();',
    sketch
)

# dashCores
sketch = re.sub(
    r'void dashCores\(uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+)\) \{',
    r'void dashCores(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  uint32_t \3 = \3_str.toInt();\n  uint32_t \4 = \4_str.toInt();\n  uint32_t \5 = \5_str.toInt();',
    sketch
)

# dashCoreFreqs
sketch = re.sub(
    r'void dashCoreFreqs\(uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+)\) \{',
    r'void dashCoreFreqs(String \1_str, String \2_str, String \3_str, String \4_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  uint32_t \3 = \3_str.toInt();\n  uint32_t \4 = \4_str.toInt();',
    sketch
)

# dashMemory
sketch = re.sub(
    r'void dashMemory\(uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+),\s*uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+)\) \{',
    r'void dashMemory(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str, String \6_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  uint32_t \3 = \3_str.toInt();\n  uint32_t \4 = \4_str.toInt();\n  uint32_t \5 = \5_str.toInt();\n  uint32_t \6 = \6_str.toInt();',
    sketch
)

# dashStorage
sketch = re.sub(
    r'void dashStorage\(uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+),\s*uint32_t ([A-Za-z0-9_]+), int32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+)\) \{',
    r'void dashStorage(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str, String \6_str, String \7_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  uint32_t \3 = \3_str.toInt();\n  uint32_t \4 = \4_str.toInt();\n  int32_t \5 = \5_str.toInt();\n  uint32_t \6 = \6_str.toInt();\n  uint32_t \7 = \7_str.toInt();',
    sketch
)

# dashNetwork
sketch = re.sub(
    r'void dashNetwork\(uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), int32_t ([A-Za-z0-9_]+), int32_t ([A-Za-z0-9_]+),\s*uint32_t ([A-Za-z0-9_]+), uint32_t ([A-Za-z0-9_]+), int32_t ([A-Za-z0-9_]+)\) \{',
    r'void dashNetwork(String \1_str, String \2_str, String \3_str, String \4_str, String \5_str, String \6_str, String \7_str) {\n  uint32_t \1 = \1_str.toInt();\n  uint32_t \2 = \2_str.toInt();\n  int32_t \3 = \3_str.toInt();\n  int32_t \4 = \4_str.toInt();\n  uint32_t \5 = \5_str.toInt();\n  uint32_t \6 = \6_str.toInt();\n  int32_t \7 = \7_str.toInt();',
    sketch
)

with open('unoq_st_dashboard/sketch/sketch.ino', 'w') as f:
    f.write(sketch)

print("Patch applied")

