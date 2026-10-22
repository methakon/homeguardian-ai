"""Wake the handset, keep it awake while USB is attached, and start the bridge."""
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ADB = ['adb', '-s', 'edd53dbd04de']

def adb(*args):
    return subprocess.check_output(ADB + list(args), text=True, timeout=25)

def nodes():
    data = adb('exec-out', 'uiautomator', 'dump', '/dev/tty')
    start, end = data.find('<?xml'), data.rfind('</hierarchy>')
    assert start >= 0 and end >= 0, 'No UI hierarchy (device offline or screen unavailable)'
    return list(ET.fromstring(data[start:end + 12]).iter('node'))

def wake():
    adb('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP')
    adb('shell', 'wm', 'dismiss-keyguard')
    adb('shell', 'am', 'start', '-n', 'org.homeguardian.edgeservice/.MainActivity')

def app_nodes():
    return [n for n in nodes() if n.get('package') == 'org.homeguardian.edgeservice']

def click_button(label):
    matches = [n for n in app_nodes()
               if n.get('class', '').endswith('Button') and n.get('text', '').upper() == label.upper()]
    assert len(matches) == 1, 'Operator button not found: ' + label
    bounds = matches[0].get('bounds') or ''
    numbers = re.findall(r'\d+', bounds)
    assert len(numbers) == 4, 'Malformed button bounds: ' + bounds
    x1, y1, x2, y2 = map(int, numbers)
    assert x2 > x1 and y2 > y1 and y1 < 1500, 'Button not reachable on screen'
    adb('shell', 'input', 'tap', str((x1 + x2) // 2), str((y1 + y2) // 2))

def scroll_down():
    adb('shell', 'input', 'swipe', '360', '1200', '360', '500', '400')

if __name__ == '__main__':
    action = sys.argv[1]
    if action == 'awake':
        adb('shell', 'svc', 'power', 'stayon', 'usb')
        wake()
        print('awake; stayon=usb while plugged in')
    elif action == 'release':
        adb('shell', 'svc', 'power', 'stayon', 'false')
        adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')
        print('stayon released; app sent to background (capture stays off)')
    elif action == 'start-bridge':
        wake()
        scroll_down()
        click_button('Start bridge')
        print('start bridge tapped')
    elif action == 'status':
        wake()
        scroll_down()
        for n in app_nodes():
            text = n.get('text', '')
            if 'bridge' in text.lower() or 'tunnel' in text.lower():
                print(text.replace('\n', ' | '))
