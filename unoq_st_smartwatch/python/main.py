import time
from arduino.app_utils import App


def loop():
    time.sleep(60)


App.run(user_loop=loop)
