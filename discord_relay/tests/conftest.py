"""Test fixtures for the VanChildSafety Discord relay.

The relay is a child-safety notification system; every test here covers a
behavior that, if broken, could either (a) drop an emergency alert or
(b) spam @everyone on a non-emergency. We construct a DiscordRelay with
all I/O mocked:

- requests.post is captured into a list and never hits the network.
- The paho MQTT client is replaced with a fake that records publishes.
- The DiscordSender worker thread is bypassed (we call _send_once and
  _process directly) so tests are deterministic and fast.
"""
import sys
import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock

import pytest

# Make relay.py importable.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))


class FakeMqttClient:
    """Minimal stand-in for paho.mqtt.Client used by the relay.

    Captures every publish so tests can assert on what would have been sent
    to the broker. on_message / on_connect / on_disconnect are real callbacks
    so the relay's handlers run exactly as in production.
    """

    def __init__(self):
        self.published = []          # list of (topic, payload, qos, retain)
        self.subscribed = []
        self.connected = False
        self.will = None
        # Plain attributes; the relay assigns bound methods to these. They
        # shadow the placeholders below.
        self.on_connect = lambda c, u, f, r, p: None
        self.on_disconnect = lambda c, u, f, r, p: None
        self.on_message = lambda c, u, m: None

    def username_pw_set(self, *a, **kw):
        pass

    def will_set(self, topic, payload, qos=1, retain=True):
        self.will = (topic, payload, qos, retain)

    def connect(self, host, port, keepalive=60):
        self.connected = True

    def disconnect(self):
        self.connected = False

    def loop_forever(self):
        pass

    def subscribe(self, topic, qos=1):
        self.subscribed.append(topic)

    def publish(self, topic, payload, qos=1, retain=False):
        self.published.append((topic, payload, qos, retain))
        # Return an MQTTMessageInfo-like object with wait_for_publish().
        return SimpleNamespace(wait_for_publish=lambda timeout=None: True)

    # Helper to inject a message as if it came from the broker.
    def deliver(self, suffix, payload, retain=False):
        topic = f"bus/{suffix}"
        msg = SimpleNamespace(
            topic=topic,
            payload=payload.encode("utf-8") if isinstance(payload, str) else payload,
            retain=retain,
        )
        # paho v2 callback signature: (client, userdata, flags, reason_code, properties)
        # for connect/disconnect, and (client, userdata, message) for message.
        # The relay's on_message accepts (client, userdata, message).
        self.on_message(self, None, msg)

    # Compatibility wrapper for paho's CallbackAPIVersion.VERSION2 constructor.
    @classmethod
    def make(cls, client_id=None, clean_session=None):
        return cls()


@pytest.fixture
def relay(monkeypatch, tmp_path):
    """Construct a DiscordRelay with env set to deterministic test values."""
    env = {
        "MQTT_BASE_TOPIC": "bus",
        "MQTT_HOST": "127.0.0.1",
        "MQTT_PORT": "1883",
        "MQTT_USERNAME": "",
        "MQTT_PASSWORD": "",
        "MQTT_CLIENT_ID": "test-relay",
        "DISCORD_BOT_TOKEN": "fake-token",
        "DISCORD_GLOBAL_CHANNEL_ID": "111111111111111111",
        "DISCORD_DEFAULT_CHILD_CHANNEL_ID": "222222222222222222",
        "DISCORD_EMERGENCY_CHANNEL_ID": "333333333333333333",
        "DISCORD_ALLOWED_MENTION": "@everyone",
        "DISTRESS_REPEAT_INTERVAL_S": "300",
        "SCHOOL_ZONE_ENTITY": "zone.school",
        "LOG_PATH": str(tmp_path / "relay.jsonl"),
    }
    for k, v in env.items():
        monkeypatch.setenv(k, v)

    # Replace paho.mqtt.Client with our fake.
    import relay as relay_mod
    monkeypatch.setattr(relay_mod.mqtt, "Client",
                        lambda *a, **kw: FakeMqttClient.make())

    r = relay_mod.DiscordRelay()
    # Don't start the sender thread; tests want deterministic synchronous
    # execution. Override sender.enqueue to synchronously run _send_once so
    # dedupe, retry, and fallback behavior all execute in-test.
    r.sender.start = lambda: None
    r.sender.stop = lambda: None
    r.sender.join = lambda timeout=None: None

    def sync_enqueue(job, block=False):
        # Bypass the queue: run the job inline. Tests assert on captured_posts
        # which is populated by the mocked requests.post.
        r.sender._process(job)
        return True

    r.sender.enqueue = sync_enqueue
    yield r

    # DiscordRelay uses a process-global named logger. Close and remove only
    # the handlers installed by this instance so tests do not leak file
    # descriptors or duplicate log lines as the suite grows.
    for handler in list(r.logger.handlers):
        r.logger.removeHandler(handler)
        handler.close()


@pytest.fixture
def captured_posts(monkeypatch):
    """Capture every requests.post call into a list."""
    posts = []

    def fake_post(url, headers=None, json=None, timeout=None):
        posts.append(SimpleNamespace(url=url, headers=headers, json=json, timeout=timeout))
        resp = MagicMock()
        resp.status_code = 200
        resp.text = '{"id":"msg-1"}'
        resp.json.return_value = {"id": "msg-1"}
        return resp

    def fake_get(url, headers=None, timeout=None):
        resp = MagicMock()
        resp.status_code = 200
        resp.json.return_value = {"id": "@me"}
        return resp

    import relay as relay_mod
    monkeypatch.setattr(relay_mod.requests, "post", fake_post)
    monkeypatch.setattr(relay_mod.requests, "get", fake_get)
    return posts


def deliver_to(relay, suffix, payload, retain=False):
    if isinstance(payload, (dict, list)):
        payload = json.dumps(payload)
    relay.client.deliver(suffix, payload, retain=retain)
