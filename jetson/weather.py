"""Fetch and cache the watch weather frame independently of the BLE loop."""

import http.client
import json
import logging
import threading
import time
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from protocol import weather_sync


LOG = logging.getLogger("hspi")
FORECAST_URL = "https://api.open-meteo.com/v1/forecast"
REFRESH_SECONDS = 30 * 60
RETRY_SECONDS = 5 * 60
FRESH_SECONDS = 2 * 60 * 60
HTTP_TIMEOUT = 8
MAX_RESPONSE_BYTES = 64 * 1024
UNKNOWN_BYTE = 255
UNKNOWN_DEGREE = -128


def rounded(value, minimum, maximum, unknown, scale=1):
    if type(value) not in (int, float) or not minimum <= value <= maximum:
        return unknown
    return int(round(value * scale))


def forecast_values(daily, name, minimum, maximum, unknown):
    values = daily.get(name)
    if not isinstance(values, list):
        return [unknown] * 3
    readings = [rounded(item, minimum, maximum, unknown) for item in values[:3]]
    return readings + [unknown] * (3 - len(readings))


def fetch_weather(latitude, longitude):
    params = {
        "latitude": latitude,
        "longitude": longitude,
        "current": "temperature_2m,relative_humidity_2m,wind_speed_10m,weather_code",
        "daily": "weather_code,temperature_2m_max,temperature_2m_min",
        "temperature_unit": "celsius",
        "wind_speed_unit": "kmh",
        "timezone": "auto",
        "forecast_days": 3,
    }
    request = Request(FORECAST_URL + "?" + urlencode(params),
                      headers={"User-Agent": "HSPI-SOS Weather Gateway/1"})
    with urlopen(request, timeout=HTTP_TIMEOUT) as response:
        body = response.read(MAX_RESPONSE_BYTES + 1)
    if len(body) > MAX_RESPONSE_BYTES:
        raise ValueError("weather response exceeded size limit")
    data = json.loads(body)
    current = data.get("current") if isinstance(data, dict) else None
    if not isinstance(current, dict):
        raise ValueError("current weather missing from response")
    temp = rounded(current.get("temperature_2m"), -127, 127, None, scale=10)
    if temp is None:
        raise ValueError("current temperature unavailable")

    daily = data.get("daily")
    if not isinstance(daily, dict):
        daily = {}
    return weather_sync(
        rounded(current.get("weather_code"), 0, 99, UNKNOWN_BYTE),
        rounded(current.get("relative_humidity_2m"), 0, 100, UNKNOWN_BYTE),
        rounded(current.get("wind_speed_10m"), 0, 254, UNKNOWN_BYTE),
        temp,
        forecast_values(daily, "weather_code", 0, 99, UNKNOWN_BYTE),
        forecast_values(daily, "temperature_2m_max", -127, 127, UNKNOWN_DEGREE),
        forecast_values(daily, "temperature_2m_min", -127, 127, UNKNOWN_DEGREE),
        int(time.time()),
    )


class WeatherFeed:
    def __init__(self, latitude, longitude, city):
        self.latitude = latitude
        self.longitude = longitude
        self.city = city or "configured location"
        self.stop = threading.Event()
        self.snapshot = None

    def start(self):
        threading.Thread(target=self.refresh, name="watch-weather", daemon=True).start()

    def close(self):
        self.stop.set()

    def current(self):
        snapshot = self.snapshot
        if snapshot is not None and time.monotonic() - snapshot[0] < FRESH_SECONDS:
            return snapshot[1]
        return None

    def refresh(self):
        while not self.stop.is_set():
            try:
                frame = fetch_weather(self.latitude, self.longitude)
            except (OSError, ValueError, http.client.HTTPException) as exc:
                LOG.warning("Weather refresh failed for %s: %s", self.city, exc)
                delay = RETRY_SECONDS
            else:
                self.snapshot = (time.monotonic(), frame)
                LOG.info("Weather refreshed for %s", self.city)
                delay = REFRESH_SECONDS
            self.stop.wait(delay)
