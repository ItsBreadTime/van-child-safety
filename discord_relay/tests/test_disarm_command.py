"""Guarded Home Assistant/Discord disarm command tests."""
import json
import threading
import time

from tests.conftest import deliver_to


def _ready(relay, state="armed"):
    relay.mqtt_connected = True
    relay.firmware_online = True
    relay.bus_state = {"state": state}
    relay.operator_user_ids = {"555555555555555555"}
    relay.operator_role_ids = {"666666666666666666"}


def test_discord_disarm_is_default_deny_and_requires_confirmation(relay):
    _ready(relay)
    ok, reason = relay.validate_discord_disarm("777777777777777777", set(), True)
    assert not ok
    assert "not an authorized" in reason

    ok, reason = relay.validate_discord_disarm(
        "555555555555555555", set(), False,
    )
    assert not ok
    assert "confirmation" in reason


def test_discord_role_authorization_and_distress_gate(relay):
    _ready(relay, state="distress_active")
    ok, reason = relay.validate_discord_disarm(
        "777777777777777777", {"666666666666666666"}, True,
    )
    assert not ok
    assert "acknowledge" in reason

    relay.bus_state = {"state": "acked"}
    ok, reason = relay.validate_discord_disarm(
        "777777777777777777", {"666666666666666666"}, True,
    )
    assert ok
    assert reason == "authorized"


def test_disarm_waits_for_matching_firmware_confirmation(relay):
    _ready(relay)
    result_holder = {}

    def invoke():
        result_holder["result"] = relay.request_disarm(
            source="discord", actor="discord_user:555555555555555555", timeout_s=2,
        )

    thread = threading.Thread(target=invoke)
    thread.start()

    deadline = time.time() + 1
    command = None
    while time.time() < deadline:
        publishes = [p for p in relay.client.published if p[0] == "bus/command"]
        if publishes:
            command = json.loads(publishes[-1][1])
            break
        time.sleep(0.01)
    assert command is not None
    assert command["action"] == "disarm"
    assert command["confirm_cabin_clear"] is True
    assert command["source"] == "discord"

    # A stale result must not complete this request.
    deliver_to(relay, "command/result", {
        "request_id": "old-request", "action": "disarm", "ok": True,
        "reason": "disarmed", "source": "discord",
    })
    assert thread.is_alive()

    deliver_to(relay, "command/result", {
        "request_id": command["request_id"], "action": "disarm", "ok": True,
        "reason": "disarmed", "source": "discord", "state": "disarmed",
    })
    deliver_to(relay, "state", {"state": "disarmed"})
    thread.join(timeout=1)
    assert not thread.is_alive()
    assert result_holder["result"]["ok"] is True
    assert result_holder["result"]["state"] == "disarmed"


def test_disarm_result_requires_matching_bus_state(relay):
    _ready(relay)
    result_holder = {}

    def invoke():
        result_holder["result"] = relay.request_disarm(
            source="discord", actor="discord_user:555555555555555555", timeout_s=0.2,
        )

    thread = threading.Thread(target=invoke)
    thread.start()

    deadline = time.time() + 1
    command = None
    while time.time() < deadline:
        publishes = [p for p in relay.client.published if p[0] == "bus/command"]
        if publishes:
            command = json.loads(publishes[-1][1])
            break
        time.sleep(0.01)
    assert command is not None

    deliver_to(relay, "command/result", {
        "request_id": command["request_id"], "action": "disarm", "ok": True,
        "reason": "disarmed", "source": "discord", "state": "disarmed",
    })
    thread.join(timeout=1)
    assert result_holder["result"]["ok"] is False
    assert result_holder["result"]["reason"] == "bus_state_confirmation_timeout"


def test_disarm_timeout_is_fail_closed(relay):
    _ready(relay)
    result = relay.request_disarm(
        source="discord", actor="discord_user:555555555555555555", timeout_s=0.01,
    )
    assert result["ok"] is False
    assert result["reason"] == "firmware_confirmation_timeout"


def test_beacon_add_payload_validation(relay):
    payload, error = relay.build_beacon_add_payload(
        id="alice_wristband",
        person_name="Alice",
        uuid_text="f7826da6-4fa2-4e98-8024-bc5b71e0893e",
        major=1,
        minor=100,
        rssi_threshold=-74,
        discord_channel_id="222222222222222222",
        home_zone_entity="zone.example_home",
        notes="red backpack",
    )
    assert error is None
    assert payload["id"] == "alice_wristband"
    assert payload["type"] == "ibeacon"
    assert payload["rssi_threshold"] == -74

    payload, error = relay.build_beacon_add_payload(
        id="bad space",
        person_name="Alice",
        uuid_text="not-a-uuid",
        major=1,
        minor=100,
    )
    assert payload is None
    assert "id must be" in error


def test_beacon_update_payload_only_sends_changed_fields(relay):
    payload, error = relay.build_beacon_update_payload(
        id="alice_wristband",
        person_name="Alice B.",
        rssi_threshold="-72",
        discord_channel_id="",
        home_zone_entity="zone.example_home",
    )
    assert error is None
    assert payload == {
        "id": "alice_wristband",
        "person_name": "Alice B.",
        "rssi_threshold": -72,
        "home_zone_entity": "zone.example_home",
    }

    payload, error = relay.build_beacon_update_payload(id="alice_wristband")
    assert payload is None
    assert "at least one field" in error


def test_beacon_config_request_waits_for_matching_firmware_result(relay):
    _ready(relay, state="active")
    result_holder = {}

    def invoke():
        result_holder["result"] = relay.request_beacon_config(
            "add",
            {"id": "alice_wristband", "person_name": "Alice"},
            source="discord",
            actor="discord_user:555555555555555555",
            timeout_s=2,
        )

    thread = threading.Thread(target=invoke)
    thread.start()

    deadline = time.time() + 1
    command = None
    while time.time() < deadline:
        publishes = [p for p in relay.client.published if p[0] == "bus/beacon/config/set"]
        if publishes:
            command = json.loads(publishes[-1][1])
            break
        time.sleep(0.01)
    assert command is not None
    assert command["action"] == "add"
    assert command["source"] == "discord"
    assert command["actor"] == "discord_user:555555555555555555"

    deliver_to(relay, "beacon/config/result", {
        "request_id": "old-request", "action": "add", "ok": True,
        "message": "Beacon added",
    })
    assert thread.is_alive()

    deliver_to(relay, "beacon/config/result", {
        "request_id": command["request_id"], "action": "add", "ok": True,
        "id": "alice_wristband", "message": "Beacon added",
    })
    thread.join(timeout=1)
    assert not thread.is_alive()
    assert result_holder["result"]["ok"] is True
    assert result_holder["result"]["id"] == "alice_wristband"


def test_beacon_config_timeout_is_fail_closed(relay):
    _ready(relay, state="active")
    result = relay.request_beacon_config(
        "list", {}, source="discord", actor="discord_user:555555555555555555",
        timeout_s=0.01,
    )
    assert result["ok"] is False
    assert result["reason"] == "firmware_confirmation_timeout"


def test_beacon_deactivate_blocked_while_armed(relay):
    _ready(relay, state="armed")

    result = relay.request_beacon_config(
        "update",
        {"id": "alice_wristband", "active": False},
        source="discord",
        actor="discord_user:555555555555555555",
        timeout_s=0.01,
    )

    assert result["ok"] is False
    assert result["reason"] == "safety_state_blocks_roster_change"
    assert "armed" in result["message"]
    assert not [p for p in relay.client.published if p[0] == "bus/beacon/config/set"]


def test_beacon_remove_blocked_when_beacon_is_near(relay):
    _ready(relay, state="active")
    relay.beacon_states["alice_wristband"] = {"state": "near"}

    result = relay.request_beacon_config(
        "remove",
        {"id": "alice_wristband"},
        source="discord",
        actor="discord_user:555555555555555555",
        timeout_s=0.01,
    )

    assert result["ok"] is False
    assert result["reason"] == "safety_state_blocks_roster_change"
    assert "near" in result["message"]
    assert not [p for p in relay.client.published if p[0] == "bus/beacon/config/set"]


def test_beacon_activate_allowed_during_armed_state(relay):
    _ready(relay, state="armed")

    result = relay.request_beacon_config(
        "update",
        {"id": "alice_wristband", "active": True},
        source="discord",
        actor="discord_user:555555555555555555",
        timeout_s=0.01,
    )

    assert result["ok"] is False
    assert result["reason"] == "firmware_confirmation_timeout"
    publishes = [p for p in relay.client.published if p[0] == "bus/beacon/config/set"]
    assert publishes
    assert json.loads(publishes[-1][1])["active"] is True
