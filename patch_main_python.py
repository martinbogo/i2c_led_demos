with open('unoq_st_dashboard/python/main.py', 'r') as f:
    lines = f.readlines()

new_lines = []
for line in lines:
    if line.startswith('from arduino.app_utils import App, Bridge'):
        new_lines.append(line)
        new_lines.append('\noriginal_notify = Bridge.notify\ndef notify_str(*args):\n    return original_notify(*[str(a) if isinstance(a, int) else a for a in args])\nBridge.notify = notify_str\n')
    else:
        new_lines.append(line)

with open('unoq_st_dashboard/python/main.py', 'w') as f:
    f.writelines(new_lines)
