"""Tests for the metrics endpoint + persistent dedupe state, both added
in relay v0.3.0. The metrics server is a small HTTP server (stdlib only)
bound to :9101 by default; the Docker HEALTHCHECK hits /health and
Prometheus scrapes /metrics."""
import json
import urllib.request
from urllib.error import HTTPError

from tests.conftest import deliver_to


def test_metrics_endpoint_returns_prometheus_format(relay):
    import relay as relay_mod
    server = relay_mod.start_metrics_server(0)
    port = server.server_address[1]
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/metrics", timeout=2) as resp:
            body = resp.read().decode("utf-8")
            assert resp.headers.get_content_type() == "text/plain"
        assert "discord_sends_total" in body
        assert "emergency_sends_total" in body
        assert "distress_received_total" in body
        assert "bus_relay_uptime_seconds" in body
        # No PII / tokens in metrics output.
        assert "fake-token" not in body
    finally:
        server.shutdown()
        server.server_close()


def test_health_endpoint_returns_200_and_unknown_path_is_404(relay):
    import relay as relay_mod
    server = relay_mod.start_metrics_server(0)
    port = server.server_address[1]
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=2) as resp:
            assert resp.status == 200
            assert resp.read() == b"ok\n"
            assert resp.headers.get_content_type() == "text/plain"
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/unknown", timeout=2)
        except HTTPError as exc:
            assert exc.code == 404
        else:
            raise AssertionError("unknown metrics path should return 404")
    finally:
        server.shutdown()
        server.server_close()


def test_metrics_counters_increment_on_send_and_dedupe(relay, captured_posts):
    import relay as relay_mod
    before = relay_mod.METRICS._counters["discord_sends_total"]
    before_skip = relay_mod.METRICS._counters["discord_send_skipped_total"]
    assert relay.send_discord(relay.global_channel, "metrics", key="metrics-key") is True
    assert relay_mod.METRICS._counters["discord_sends_total"] > before
    assert relay.send_discord(relay.global_channel, "metrics", key="metrics-key") is False
    assert relay_mod.METRICS._counters["discord_send_skipped_total"] == before_skip + 1
    assert len(captured_posts) == 1


# ---------------------------------------------------------------------------
# Persistent dedupe state
# ---------------------------------------------------------------------------
def test_state_persisted_across_restart(relay, captured_posts, monkeypatch, tmp_path):
    """If the relay restarts mid-incident, it should not re-send @everyone
    for an already-known distress id within the repeat interval."""
    state_path = relay._state_path
    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "distress/state", {
        "id": "distress-persist-1", "severity": "emergency", "state": "active",
        "trigger": "co2_rise"})
    assert state_path.exists()
    saved = json.loads(state_path.read_text())
    assert saved["active_distress_id"] == "distress-persist-1"
    assert saved["distress_last_sent_at"] > 0

    # Construct a fresh relay (simulating restart) - it should load the state.
    import relay as relay_mod
    monkeypatch.setattr(relay_mod.mqtt, "Client", lambda *a, **kw: __import__(
        "tests.conftest", fromlist=["FakeMqttClient"]).FakeMqttClient.make())
    new_relay = relay_mod.DiscordRelay()
    new_relay.sender.start = lambda: None
    new_relay.sender.stop = lambda: None
    new_relay.sender.join = lambda timeout=None: None
    new_relay.sender.enqueue = lambda job, block=False: new_relay.sender._process(job) or True
    assert new_relay.active_distress_id == "distress-persist-1"
    before_posts = len(captured_posts)
    before_deduped = relay_mod.METRICS._counters["distress_deduped_total"]
    deliver_to(new_relay, "distress/state", {
        "id": "distress-persist-1", "severity": "emergency", "state": "active",
        "trigger": "co2_rise"})
    assert len(captured_posts) == before_posts
    assert relay_mod.METRICS._counters["distress_deduped_total"] == before_deduped + 1

    for handler in list(new_relay.logger.handlers):
        new_relay.logger.removeHandler(handler)
        handler.close()


def test_corrupt_state_file_does_not_crash(relay, monkeypatch):
    # Write a corrupt state file before construction.
    relay._state_path.write_text("{corrupt json")
    # _load_persistent_state already ran in __init__; reload now to
    # exercise the JSONDecodeError path explicitly.
    relay._load_persistent_state()
    # Relay should remain functional.
    assert relay.active_distress_id is None
