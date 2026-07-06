"""Discord transport, queue, and lifecycle tests.

Behavioral rule tests use a synchronous sender for determinism. This module
tests the asynchronous boundary and transport failures that fixture bypasses.
"""
import queue
import json
import threading
import time
from types import SimpleNamespace

import pytest
import requests

from relay import DiscordOperatorClient, DiscordSender, SendJob


class AlwaysFullQueue:
    def __init__(self):
        self.calls = []

    def put(self, job, timeout=None):
        self.calls.append(("put", timeout))
        raise queue.Full

    def put_nowait(self, job):
        self.calls.append(("put_nowait", None))
        raise queue.Full


@pytest.mark.parametrize(
    ("block", "expected_call"),
    [(False, ("put_nowait", None)), (True, ("put", 10))],
)
def test_enqueue_reports_full_queue(relay, monkeypatch, block, expected_call):
    full_queue = AlwaysFullQueue()
    sender = DiscordSender(relay)
    sender.queue = full_queue
    logs = []
    monkeypatch.setattr(relay, "log", lambda event, **fields: logs.append((event, fields)))

    accepted = sender.enqueue(
        SendJob(relay.global_channel, "message", emergency=block, key=None),
        block=block,
    )

    assert accepted is False
    assert full_queue.calls == [expected_call]
    assert logs[0][0] == "send_queue_full"
    assert logs[0][1]["emergency"] is block


def test_worker_processes_enqueued_job(relay, monkeypatch):
    sender = DiscordSender(relay)
    processed = threading.Event()
    monkeypatch.setattr(sender, "_process", lambda job: processed.set())
    monkeypatch.setattr(sender, "_liveness_probe", lambda: None)

    sender.start()
    try:
        assert sender.enqueue(
            SendJob(relay.global_channel, "message", emergency=False, key=None),
            block=False,
        ) is True
        assert processed.wait(timeout=1)
    finally:
        sender.stop()
        sender.join(timeout=2)
    assert not sender.is_alive()


@pytest.mark.parametrize("status_code, expected", [(200, True), (401, False)])
def test_liveness_probe_updates_discord_readiness(relay, monkeypatch, status_code, expected):
    relay.sender.discord_ready = not expected
    monkeypatch.setattr(
        "relay.requests.get",
        lambda *args, **kwargs: SimpleNamespace(status_code=status_code),
    )

    relay.sender._liveness_probe()

    assert relay.sender.discord_ready is expected


def test_successful_send_clears_error_as_json(relay, monkeypatch):
    monkeypatch.setattr(
        "relay.requests.post",
        lambda *args, **kwargs: SimpleNamespace(
            status_code=200,
            json=lambda: {"id": "msg-1"},
            text='{"id":"msg-1"}',
        ),
    )

    assert relay.sender._send_once(
        SendJob(relay.global_channel, "message", emergency=False, key=None)
    ) is True
    errors = [payload for topic, payload, _qos, _retain in relay.client.published
              if topic == "bus/relay/discord/error"]
    assert errors
    assert json.loads(errors[-1])["status"] == "clear"


def test_corrupt_persistent_sent_keys_are_ignored(relay, tmp_path):
    state_path = tmp_path / "relay_state.json"
    state_path.write_text(json.dumps({
        "sent_keys": [
            ["valid-key", 9999999999.0],
            ["bad-ts", "not-a-float"],
            ["too-short"],
            {"unexpected": "shape"},
            [42, 123.0],
        ],
        "active_distress_id": "distress-1",
        "distress_last_sent_at": 456.0,
    }), encoding="utf-8")

    relay._sent_keys.clear()
    relay._state_path = state_path
    relay._load_persistent_state()

    assert list(relay._sent_keys) == [("valid-key", 9999999999.0)]
    assert relay.active_distress_id == "distress-1"
    assert relay.distress_last_sent_at == 456.0


def test_bad_persistent_distress_timestamp_is_ignored(relay, tmp_path):
    state_path = tmp_path / "relay_state.json"
    state_path.write_text(json.dumps({
        "sent_keys": [],
        "active_distress_id": "distress-1",
        "distress_last_sent_at": "not-a-number",
    }), encoding="utf-8")

    relay.distress_last_sent_at = 123.0
    relay._state_path = state_path
    relay._load_persistent_state()

    assert relay.active_distress_id == "distress-1"
    assert relay.distress_last_sent_at == 0.0


def test_liveness_probe_handles_network_exception(relay, monkeypatch):
    relay.sender.discord_ready = True

    def fail(*args, **kwargs):
        raise requests.ConnectionError("offline")

    monkeypatch.setattr("relay.requests.get", fail)
    relay.sender._liveness_probe()
    assert relay.sender.discord_ready is False


def test_rate_limit_retries_after_bounded_retry_after(relay, monkeypatch):
    attempts = []
    sleeps = []

    def post(*args, **kwargs):
        attempts.append(kwargs)
        if len(attempts) == 1:
            return SimpleNamespace(
                status_code=429,
                json=lambda: {"retry_after": 90},
                text="rate limited",
            )
        return SimpleNamespace(status_code=200, json=lambda: {}, text="ok")

    monkeypatch.setattr("relay.requests.post", post)
    monkeypatch.setattr("relay.time.sleep", sleeps.append)

    assert relay.sender._send_once(
        SendJob(relay.global_channel, "message", emergency=False, key=None)
    ) is True
    assert len(attempts) == 2
    assert sleeps == [30.0]


def test_rate_limit_retry_budget_is_bounded(relay, monkeypatch):
    attempts = []
    monkeypatch.setattr(
        "relay.requests.post",
        lambda *args, **kwargs: (
            attempts.append(kwargs)
            or SimpleNamespace(
                status_code=429,
                json=lambda: {"retry_after": 0},
                text="rate limited",
            )
        ),
    )
    monkeypatch.setattr("relay.time.sleep", lambda _delay: None)

    result = relay.sender._send_once(
        SendJob(relay.global_channel, "message", emergency=False, key=None),
        fallback_attempted=True,
    )

    assert result is False
    assert len(attempts) == 3
    errors = [payload for topic, payload, _qos, _retain in relay.client.published
              if topic == "bus/relay/discord/error"]
    assert any("rate limit retry budget exhausted" in payload for payload in errors)


def test_request_exception_falls_back_to_global_channel(relay, monkeypatch):
    attempts = []

    def post(url, **kwargs):
        attempts.append((url, kwargs["json"]["content"]))
        if len(attempts) == 1:
            raise requests.ConnectionError("child channel unavailable")
        return SimpleNamespace(status_code=200, json=lambda: {}, text="ok")

    monkeypatch.setattr("relay.requests.post", post)
    job = SendJob("444444444444444444", "Alice entered.", emergency=False, key="entry")

    assert relay.sender._send_once(job) is False
    assert len(attempts) == 2
    assert "444444444444444444" in attempts[0][0]
    assert relay.global_channel in attempts[1][0]
    assert attempts[1][1].startswith("Notification fallback:")


def test_emergency_exhaustion_attempts_four_times(relay, monkeypatch):
    attempts = []
    sleeps = []

    def post(*args, **kwargs):
        attempts.append(kwargs)
        return SimpleNamespace(status_code=503, json=lambda: {}, text="unavailable")

    monkeypatch.setattr("relay.requests.post", post)
    monkeypatch.setattr("relay.time.sleep", sleeps.append)

    relay.sender._send_with_retry(
        SendJob(relay.emergency_channel, "@everyone emergency", emergency=True, key=None)
    )

    assert len(attempts) == 4
    assert sleeps == [1, 5, 15]
    errors = [payload for topic, payload, _qos, _retain in relay.client.published
              if topic == "bus/relay/discord/error"]
    assert any("failed after 4 attempts" in payload for payload in errors)


def test_emergency_success_callback_only_runs_after_delivery(relay, monkeypatch):
    callbacks = []
    monkeypatch.setattr("relay.requests.post", lambda *args, **kwargs: SimpleNamespace(
        status_code=200,
        json=lambda: {},
        text="ok",
    ))

    relay.sender._process(SendJob(
        relay.emergency_channel,
        "@everyone emergency",
        emergency=True,
        key=None,
        on_success=lambda job: callbacks.append((job.channel_id, time.time())),
    ))

    assert callbacks and callbacks[0][0] == relay.emergency_channel


def test_emergency_success_callback_not_run_after_retry_exhaustion(relay, monkeypatch):
    callbacks = []
    monkeypatch.setattr("relay.requests.post", lambda *args, **kwargs: SimpleNamespace(
        status_code=503,
        json=lambda: {},
        text="unavailable",
    ))
    monkeypatch.setattr("relay.time.sleep", lambda _delay: None)

    relay.sender._process(SendJob(
        relay.emergency_channel,
        "@everyone emergency",
        emergency=True,
        key=None,
        on_success=lambda job: callbacks.append(job.channel_id),
    ))

    assert callbacks == []


@pytest.mark.parametrize(
    ("attribute", "value", "channel", "expected_error"),
    [
        ("discord_token", "", "111111111111111111", "not configured"),
        ("global_channel", "", "", "No Discord channel"),
        ("global_channel", "invalid", "invalid", "Invalid Discord channel"),
    ],
)
def test_invalid_send_configuration_is_rejected(
        relay, captured_posts, attribute, value, channel, expected_error):
    setattr(relay, attribute, value)
    if attribute == "global_channel" and value == "":
        relay.default_child_channel = ""
    relay.sender._process(SendJob(channel, "message", emergency=False, key=None))

    assert captured_posts == []
    errors = [payload for topic, payload, _qos, _retain in relay.client.published
              if topic == "bus/relay/discord/error"]
    assert any(expected_error in payload for payload in errors)


@pytest.mark.parametrize(
    ("raw", "expected"),
    [
        ("@here", {"parse": ["here"]}),
        ("<@&123456789>", {"roles": ["123456789"]}),
        ("unexpected", {"parse": ["everyone"]}),
    ],
)
def test_emergency_allowed_mentions_variants(relay, raw, expected):
    relay.allowed_mention_raw = raw
    assert relay.allowed_mentions_payload(emergency=True) == expected


def test_run_configures_lwt_and_connects(relay, monkeypatch):
    started = []
    monkeypatch.setattr("relay.start_metrics_server", lambda port: started.append(port))
    monkeypatch.setattr(relay.sender, "start", lambda: started.append("sender"))

    relay.run()

    assert relay.client.will[0] == "bus/relay/discord/status"
    assert relay.client.will[2:] == (1, True)
    assert relay.client.connected is True
    assert started == [9101, "sender"]


def test_disconnect_updates_connection_state(relay):
    relay.mqtt_connected = True
    relay.on_disconnect(relay.client, None, None, "network lost", None)
    assert relay.mqtt_connected is False


def test_rejected_mqtt_connack_does_not_mark_online_or_subscribe(relay):
    relay.on_connect(relay.client, None, None, 5, None)

    assert relay.mqtt_connected is False
    assert relay.client.subscribed == []
    statuses = [payload for topic, payload, _qos, _retain in relay.client.published
                if topic == "bus/relay/discord/status"]
    assert statuses == []


def test_mqtt_connect_subscribes_to_ha_location_state(relay):
    relay.on_connect(relay.client, None, None, 0, None)

    assert "bus/ha/location/state" in relay.client.subscribed


def test_operator_command_status_requires_gateway_ready_and_thread_alive(relay):
    relay.command_client = object()
    relay.command_gateway_ready = True
    relay.command_thread = SimpleNamespace(is_alive=lambda: True)
    relay.publish_status()

    statuses = [json.loads(payload) for topic, payload, _qos, _retain in relay.client.published
                if topic == "bus/relay/discord/status"]
    assert statuses[-1]["operator_commands_enabled"] is True

    relay.command_gateway_ready = False
    relay.publish_status()
    statuses = [json.loads(payload) for topic, payload, _qos, _retain in relay.client.published
                if topic == "bus/relay/discord/status"]
    assert statuses[-1]["operator_commands_enabled"] is False
    assert statuses[-1]["operator_gateway_ready"] is False


def test_role_ids_include_raw_interaction_member_roles():
    user = SimpleNamespace(
        _roles=[666666666666666666, "777777777777777777"],
        roles=[],
    )

    assert DiscordOperatorClient.role_ids_for_user(user) == {
        "666666666666666666",
        "777777777777777777",
    }


def test_role_ids_include_hydrated_member_roles():
    role = SimpleNamespace(id=888888888888888888)
    user = SimpleNamespace(roles=[role])

    assert DiscordOperatorClient.role_ids_for_user(user) == {"888888888888888888"}


def test_stop_publishes_offline_and_disconnects(relay):
    relay.client.connected = True

    with pytest.raises(SystemExit) as exc:
        relay.stop()

    assert exc.value.code == 0
    assert relay.client.connected is False
    statuses = [payload for topic, payload, _qos, _retain in relay.client.published
                if topic == "bus/relay/discord/status"]
    assert any('"status":"offline"' in payload for payload in statuses)
