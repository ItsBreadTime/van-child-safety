"""End-to-end behavioral tests for the Discord relay.

Every test exercises a specific notification rule, fallback path, or
dedupe behavior. Tests that mock requests.post assert on what would have been
sent to Discord.
"""
import json
from types import SimpleNamespace

from tests.conftest import deliver_to


# ===========================================================================
# Roster loading
# ===========================================================================
def test_roster_loaded_from_retained_message(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {
        "version": 1, "max_records": 20, "count": 1,
        "records": [{
            "id": "alice_wristband", "person_name": "Alice", "active": True,
            "discord_channel_id": "444444444444444444",
            "home_zone_entity": "zone.example_home",
        }],
    }, retain=True)
    assert "alice_wristband" in relay.roster
    assert relay.roster["alice_wristband"]["person_name"] == "Alice"
    assert relay.roster_loaded is True
    # Roster received should not post to Discord.
    assert captured_posts == []


def test_roster_ignores_invalid_beacon_ids(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {
        "records": [
            {"id": "bad space!id", "person_name": "Bad"},
            {"id": "../../../etc/passwd", "person_name": "PathTraversal"},
            {"id": "valid_one", "person_name": "Good"},
        ],
    }, retain=True)
    assert "bad space!id" not in relay.roster
    assert "../../../etc/passwd" not in relay.roster
    assert "valid_one" in relay.roster


def test_roster_update_prunes_removed_and_inactive_beacon_state(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice"},
        {"id": "bob", "person_name": "Bob"},
        {"id": "cara", "person_name": "Cara"},
    ]})
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "beacon/alice/state", {"state": "near"})
    deliver_to(relay, "beacon/bob/state", {"state": "near"})
    deliver_to(relay, "beacon/cara/state", {"state": "near"})

    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice"},
        {"id": "bob", "person_name": "Bob", "active": False},
    ]})

    assert "alice" in relay.beacon_states
    assert "bob" not in relay.beacon_states
    assert "cara" not in relay.beacon_states
    captured_posts.clear()
    deliver_to(relay, "beacon/bob/state", {"state": "near"})
    assert captured_posts == []


# ===========================================================================
# Child entry/exit during ACTIVE / EXIT_GRACE
# ===========================================================================
def test_child_enters_bus_during_active(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {
        "records": [{"id": "alice", "person_name": "Alice",
                     "discord_channel_id": "444444444444444444"}],
    }, retain=True)
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "beacon/alice/state", {"state": "near", "timestamp": "2026-06-17T08:00:00Z"})
    assert len(captured_posts) == 1
    assert "entered the bus" in captured_posts[0].json["content"]
    assert "Alice" in captured_posts[0].json["content"]
    assert captured_posts[0].json["embeds"][0]["title"] == "Child entered bus"
    fields = {field["name"]: field["value"] for field in captured_posts[0].json["embeds"][0]["fields"]}
    assert fields["Child"] == "Alice"
    assert fields["Beacon ID"] == "`alice`"
    # No @everyone on routine messages.
    assert captured_posts[0].json["allowed_mentions"] == {"parse": []}


def test_child_enters_bus_during_exit_grace(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice"}]})
    deliver_to(relay, "state", {"state": "exit_grace"})
    deliver_to(relay, "beacon/alice/state", {"state": "near"})
    assert len(captured_posts) == 1
    assert captured_posts[0].json["content"] == "Alice entered the bus."
    assert relay.default_child_channel in captured_posts[0].url
    assert captured_posts[0].json["allowed_mentions"] == {"parse": []}


def test_late_bus_state_does_not_swallow_child_entry(relay, captured_posts):
    """Original bug: if bus/beacon/alice/state=near arrives BEFORE
    bus/state=active, the entry notification was silently dropped. The
    late-state race fix queues the transition until bus/state is known."""
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice"}]})
    # Beacon transition arrives first, bus state not yet seen.
    deliver_to(relay, "beacon/alice/state", {"state": "near", "timestamp": "t1"})
    # No notification yet - we're queueing.
    assert captured_posts == []
    # bus/state arrives later and releases the queued transition.
    deliver_to(relay, "state", {"state": "active"})
    assert len(captured_posts) == 1
    assert "entered the bus" in captured_posts[0].json["content"]
    fields = {field["name"]: field["value"]
              for field in captured_posts[0].json["embeds"][0]["fields"]}
    assert fields["Transition"] == "unknown -> near"


def test_child_exits_bus_during_active(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "discord_channel_id": "444444444444444444"}]})
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "beacon/alice/state", {"state": "near"})
    captured_posts.clear()
    deliver_to(relay, "beacon/alice/state", {"state": "far"})
    assert any("exited the bus" in p.json["content"] for p in captured_posts)


def test_far_then_absent_does_not_double_send_exit(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "discord_channel_id": "444444444444444444"}]})
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "beacon/alice/state", {"state": "near", "timestamp": "t1"})
    captured_posts.clear()

    deliver_to(relay, "beacon/alice/state", {"state": "far", "timestamp": "t2"})
    deliver_to(relay, "beacon/alice/state", {"state": "absent", "timestamp": "t3"})

    assert [p.json["content"] for p in captured_posts] == ["Alice exited the bus."]


def test_rssi_far_then_absent_sends_exit(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "discord_channel_id": "444444444444444444"}]})
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "near", "rssi": -62, "timestamp": "2026-07-05T05:53:48Z"})
    captured_posts.clear()

    deliver_to(relay, "beacon/alice/state", {
        "state": "far", "rssi": -82, "timestamp": "2026-07-05T05:53:50Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "absent", "rssi": 0, "timestamp": "2026-07-05T05:54:19Z"})

    assert [p.json["content"] for p in captured_posts] == ["Alice exited the bus."]


def test_beacon_attendance_dedupe_uses_beacon_event_timestamp(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "discord_channel_id": "444444444444444444"}]})
    deliver_to(relay, "state", {"state": "active", "timestamp": "bus-cycle-1"})

    deliver_to(relay, "beacon/alice/state", {
        "state": "near", "timestamp": "2026-07-03T15:53:09Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "absent", "timestamp": "2026-07-03T15:53:56Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "near", "timestamp": "2026-07-03T16:00:17Z"})

    contents = [p.json["content"] for p in captured_posts]
    assert contents == [
        "Alice entered the bus.",
        "Alice exited the bus.",
        "Alice entered the bus.",
    ]


def test_beacon_attendance_still_dedupes_same_event_timestamp(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "discord_channel_id": "444444444444444444"}]})
    deliver_to(relay, "state", {"state": "active", "timestamp": "bus-cycle-1"})

    payload = {"state": "near", "timestamp": "2026-07-03T15:53:09Z"}
    relay._emit_beacon_transition("alice", "absent", "near", payload)
    relay._emit_beacon_transition("alice", "absent", "near", payload)

    assert [p.json["content"] for p in captured_posts] == ["Alice entered the bus."]


def test_beacon_near_far_chatter_does_not_duplicate_entry(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "discord_channel_id": "444444444444444444"}]})
    deliver_to(relay, "state", {"state": "active", "timestamp": "bus-cycle-1"})

    deliver_to(relay, "beacon/alice/state", {
        "state": "far", "rssi": -80, "timestamp": "2026-07-03T15:53:00Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "near", "rssi": -75, "timestamp": "2026-07-03T15:53:09Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "far", "rssi": -76, "timestamp": "2026-07-03T15:53:12Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "near", "rssi": -72, "timestamp": "2026-07-03T15:53:18Z"})

    assert [p.json["content"] for p in captured_posts] == ["Alice entered the bus."]


def test_beacon_absent_resets_entry_dedupe(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "discord_channel_id": "444444444444444444"}]})
    deliver_to(relay, "state", {"state": "active", "timestamp": "bus-cycle-1"})

    deliver_to(relay, "beacon/alice/state", {
        "state": "near", "timestamp": "2026-07-03T15:53:09Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "far", "timestamp": "2026-07-03T15:53:12Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "absent", "timestamp": "2026-07-03T15:53:56Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "near", "timestamp": "2026-07-03T16:00:17Z"})

    assert [p.json["content"] for p in captured_posts] == [
        "Alice entered the bus.",
        "Alice exited the bus.",
        "Alice entered the bus.",
    ]


def test_failed_entry_send_does_not_poison_attendance_dedupe(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "discord_channel_id": "444444444444444444"}]})
    deliver_to(relay, "state", {"state": "active", "timestamp": "bus-cycle-1"})

    relay.discord_token = ""
    deliver_to(relay, "beacon/alice/state", {
        "state": "near", "timestamp": "2026-07-03T15:53:09Z"})
    assert captured_posts == []

    relay.discord_token = "fake-token"
    deliver_to(relay, "beacon/alice/state", {
        "state": "far", "timestamp": "2026-07-03T15:53:12Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "near", "timestamp": "2026-07-03T15:53:18Z"})

    assert [p.json["content"] for p in captured_posts] == ["Alice entered the bus."]


def test_child_exited_at_school(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice"}]})
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "ha/location/event", {
        "event": "zone_enter", "zone_entity": "zone.school", "zone_name": "School"})
    deliver_to(relay, "beacon/alice/state", {"state": "near"})
    captured_posts.clear()
    deliver_to(relay, "beacon/alice/state", {"state": "far"})
    assert any("exited at school" in p.json["content"] for p in captured_posts)


def test_child_reached_home(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "home_zone_entity": "zone.example_home"}]})
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "ha/location/event", {
        "event": "zone_enter", "zone_entity": "zone.example_home", "zone_name": "Example Home"})
    deliver_to(relay, "beacon/alice/state", {"state": "near"})
    captured_posts.clear()
    deliver_to(relay, "beacon/alice/state", {"state": "far"})
    assert any("reached home" in p.json["content"] for p in captured_posts)


def test_child_reached_home_from_ha_tracker_state(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "home_zone_entity": "zone.example_home"}]})
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "ha/location/state", {
        "tracker_state": "Alice",
        "latitude": 0.0,
        "longitude": 0.0,
    })
    deliver_to(relay, "beacon/alice/state", {
        "state": "near", "rssi": -60, "timestamp": "2026-07-05T05:54:59Z"})
    captured_posts.clear()

    deliver_to(relay, "beacon/alice/state", {
        "state": "far", "rssi": -84, "timestamp": "2026-07-05T05:55:07Z"})
    deliver_to(relay, "beacon/alice/state", {
        "state": "absent", "rssi": 0, "timestamp": "2026-07-05T05:55:30Z"})

    assert [p.json["content"] for p in captured_posts] == ["Alice reached home."]


def test_child_entry_during_armed_does_not_send_attendance(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice"}]})
    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "beacon/alice/state", {"state": "near"})
    # Near while armed should not produce an "entered" message - that's
    # the distress path, not the attendance path. (Distress is published
    # separately via bus/distress/state, which has its own test.)
    assert all("entered the bus" not in p.json["content"] for p in captured_posts)


# ===========================================================================
# School zone events
# ===========================================================================
def test_school_zone_enter_sends_global_message(relay, captured_posts):
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "ha/location/event", {
        "event": "zone_enter", "zone_entity": "zone.school", "zone_name": "School"})
    assert any("reached school" in p.json["content"] for p in captured_posts)


def test_school_zone_enter_dedupes_retained_replay(relay, captured_posts):
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "ha/location/event", {
        "event": "zone_enter", "zone_entity": "zone.school", "zone_name": "School"},
        retain=True)
    # Retained event on startup should not produce a duplicate message.
    assert all("reached school" not in p.json["content"] for p in captured_posts)


def test_school_zone_leave_with_child_near_warns_with_names(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice"},
        {"id": "bob", "person_name": "Bob"}]})
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "ha/location/event", {
        "event": "zone_enter", "zone_entity": "zone.school", "zone_name": "School"})
    deliver_to(relay, "beacon/alice/state", {"state": "near"})
    deliver_to(relay, "beacon/bob/state", {"state": "near"})
    deliver_to(relay, "ha/location/event", {
        "event": "zone_leave", "zone_entity": "zone.school", "zone_name": "School"})
    msgs = [p.json["content"] for p in captured_posts]
    assert any("left school" in m for m in msgs), msgs
    assert any("Alice" in m and "Bob" in m for m in msgs), msgs
    # No @everyone for warnings.
    for p in captured_posts:
        assert p.json["allowed_mentions"] == {"parse": []}


# ===========================================================================
# Missed home drop-off
# ===========================================================================
def test_child_home_zone_leave_with_child_still_near_warns(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice",
         "home_zone_entity": "zone.example_home"}]})
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "ha/location/event", {
        "event": "zone_enter", "zone_entity": "zone.example_home"})
    deliver_to(relay, "beacon/alice/state", {"state": "near"})
    deliver_to(relay, "ha/location/event", {
        "event": "zone_leave", "zone_entity": "zone.example_home"})
    msgs = [p.json["content"] for p in captured_posts]
    assert any("missed home drop-off" in m for m in msgs), msgs
    # Both global and child channel sends.
    assert len([p for p in captured_posts if "alice" in p.url or "missed" in p.json["content"]]) >= 2


# ===========================================================================
# Bus clear
# ===========================================================================
def test_bus_clear_sends_global_message_once(relay, captured_posts):
    deliver_to(relay, "state", {"state": "armed", "timestamp": "cycle-1"})
    deliver_to(relay, "event", {"event": "bus_clear", "timestamp": "ts1"})
    assert any("Bus clear" in p.json["content"] for p in captured_posts)
    captured_posts.clear()
    # Re-delivered same cycle should not duplicate.
    deliver_to(relay, "event", {"event": "bus_clear", "timestamp": "ts1"})
    assert all("Bus clear" not in p.json["content"] for p in captured_posts)


def test_bus_clear_dedupes_retained_replay(relay, captured_posts):
    deliver_to(relay, "event", {"event": "bus_clear", "timestamp": "ts1"}, retain=True)
    assert all("Bus clear" not in p.json["content"] for p in captured_posts)


# ===========================================================================
# Advisories (beacon_silent_while_armed, sensor_fault)
# ===========================================================================
def test_beacon_silent_while_armed_advisory(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice"}]})
    deliver_to(relay, "state", {"state": "armed", "timestamp": "cycle-1"})
    deliver_to(relay, "event", {
        "event": "beacon_silent_while_armed", "beacon_id": "alice"})
    assert any("silent" in p.json["content"].lower() and "alice" in p.json["content"].lower()
               for p in captured_posts)
    # No @everyone for advisories.
    for p in captured_posts:
        assert p.json["allowed_mentions"] == {"parse": []}


def test_sensor_fault_advisory(relay, captured_posts):
    deliver_to(relay, "state", {"state": "armed", "timestamp": "cycle-1"})
    deliver_to(relay, "event", {"event": "sensor_fault"})
    assert any("sensor" in p.json["content"].lower() for p in captured_posts)


# ===========================================================================
# Ignition source (ESP-NOW heartbeat remote) states
# ===========================================================================
def test_ignition_source_fields_in_bus_state(relay, captured_posts):
    """bus/state must carry ignition_source and ignition_source_status so the
    relay can surface remote health. Exercises the ESPNOW-timeout path:
    active -> exit_grace transition driven by heartbeat timeout (stale)."""
    deliver_to(relay, "state", {
        "state": "active",
        "ignition_on": True,
        "ignition_source": "espnow",
        "ignition_source_status": "ok",
        "ignition_espnow": {
            "last_heartbeat_age_s": 2,
            "remote_uptime_s": 5400,
            "remote_battery_mv": 3310,
            "seq": 2700,
            "missed_seq": 0,
        },
        "timestamp": "2026-06-23T08:00:00Z",
    })
    # Source + status fields are accepted without error; relay tracks state.
    assert relay.bus_state["ignition_source"] == "espnow"
    assert relay.bus_state["ignition_source_status"] == "ok"


def test_ignition_off_after_espnow_timeout_logs_only(relay, captured_posts):
    """When the remote stops heartbeating, firmware declares ignition off
    and emits ignition_off event -> exit_grace. The relay logs it but does
    NOT raise a distress or @everyone just because ignition went off."""
    deliver_to(relay, "state", {
        "state": "active",
        "ignition_on": True,
        "ignition_source": "espnow",
        "ignition_source_status": "ok",
        "timestamp": "2026-06-23T08:00:00Z",
    })
    # Remote goes silent -> firmware flips ignition off after 10 s timeout.
    deliver_to(relay, "state", {
        "state": "exit_grace",
        "ignition_on": False,
        "ignition_source": "espnow",
        "ignition_source_status": "stale",
        "ignition_espnow": {
            "last_heartbeat_age_s": 15,
            "missed_seq": 7,
        },
        "timestamp": "2026-06-23T08:00:15Z",
    })
    deliver_to(relay, "event", {"event": "ignition_off", "state": "exit_grace"})
    # No emergency alert for ignition-off alone; just a state transition.
    for p in captured_posts:
        assert p.json["allowed_mentions"] == {"parse": []}
        assert "stuck" not in p.json["content"].lower()


def test_ignition_source_status_overridden_is_distinct(relay, captured_posts):
    """When bus_empty_on override is active, ignition_source_status reports
    'overridden' so operators can see the override in HA/the relay."""
    deliver_to(relay, "state", {
        "state": "armed",
        "ignition_on": False,
        "ignition_source": "espnow",
        "ignition_source_status": "overridden",
        "timestamp": "2026-06-23T08:01:00Z",
    })
    assert relay.bus_state["ignition_source_status"] == "overridden"


# ===========================================================================
# Distress (emergency @everyone with ack gate)
# ===========================================================================
def test_emergency_distress_mentions_everyone(relay, captured_posts):
    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "ha/location/state", {
        "source": "home_assistant",
        "tracker_entity": "person.bus_tracker",
        "tracker_state": "School",
        "latitude": 0.0,
        "longitude": 0.0,
        "gps_accuracy": 3,
    }, retain=True)
    deliver_to(relay, "distress/state", {
        "id": "distress-001", "severity": "emergency", "state": "active",
        "trigger": "beacon_near",
        "person": {"id": "alice", "person_name": "Alice"},
        "evidence": {"co2": 731, "ld2412_target": True},
        "gps": {"valid": True, "latitude": 0.0, "longitude": 0.0},
    })
    assert any("@everyone" in p.json["content"] for p in captured_posts), \
        "Emergency message must mention @everyone"
    assert any(p.json["allowed_mentions"] == {"parse": ["everyone"]} for p in captured_posts), \
        "Emergency allowed_mentions must enable everyone parse"
    assert any("Alice" in p.json["content"] for p in captured_posts)
    emergency_post = next(p for p in captured_posts if "@everyone" in p.json["content"])
    assert emergency_post.json["embeds"][0]["title"] == "Emergency: possible child left in bus"
    assert "CO2:" not in emergency_post.json["content"]
    fields = {field["name"]: field["value"] for field in emergency_post.json["embeds"][0]["fields"]}
    assert fields["Child / beacon"] == "Alice (`alice`)"
    assert fields["Trigger"] == "Beacon near"
    assert "731 ppm" in fields["CO2"]
    assert "target yes" in fields["mmWave"]
    assert "`alice`" in fields["Beacon evidence"]
    assert "0.0, 0.0" in fields["HA location"]
    assert "School" in fields["HA location"]
    assert "GPS" not in fields
    # Sent to the emergency channel specifically.
    assert any("333333333333333333" in p.url for p in captured_posts)


def test_emergency_distress_uses_ha_location_not_firmware_gps(relay, captured_posts):
    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "ha/location/state", {
        "tracker_entity": "person.bus_tracker",
        "tracker_state": "home",
        "latitude": 1.23,
        "longitude": 4.56,
    })
    deliver_to(relay, "distress/state", {
        "id": "distress-ha-location", "severity": "emergency", "state": "active",
        "trigger": "co2_rise",
        "gps": {"valid": True, "latitude": 99.9, "longitude": 88.8},
    })

    emergency_post = next(p for p in captured_posts if "@everyone" in p.json["content"])
    fields = {field["name"]: field["value"] for field in emergency_post.json["embeds"][0]["fields"]}
    assert fields["HA location"].startswith("1.23, 4.56")
    assert "99.9" not in json.dumps(fields)


def test_emergency_distress_retries_after_send_failure(relay, monkeypatch):
    import relay as relay_mod

    attempts = []

    def fake_post(url, headers=None, json=None, timeout=None):
        attempts.append(SimpleNamespace(url=url, headers=headers, json=json, timeout=timeout))
        resp = SimpleNamespace()
        resp.status_code = 500 if len(attempts) == 1 else 200
        resp.text = "server error" if len(attempts) == 1 else '{"id":"msg-2"}'
        resp.json = lambda: {"id": "msg-2"}
        return resp

    monkeypatch.setattr(relay_mod.requests, "post", fake_post)
    monkeypatch.setattr(relay_mod.time, "sleep", lambda *_args, **_kwargs: None)

    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "distress/state", {
        "id": "distress-retry-001", "severity": "emergency", "state": "active",
        "trigger": "co2_rise"})

    assert len(attempts) == 2
    assert attempts[0].json["allowed_mentions"] == {"parse": ["everyone"]}
    assert attempts[1].json["allowed_mentions"] == {"parse": ["everyone"]}


def test_distress_dedupe_by_id_within_repeat_interval(relay, captured_posts):
    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "distress/state", {
        "id": "distress-001", "severity": "emergency", "state": "active",
        "trigger": "co2_rise"})
    first_count = len(captured_posts)
    # Re-deliver same distress id immediately -> dedupe.
    deliver_to(relay, "distress/state", {
        "id": "distress-001", "severity": "emergency", "state": "active",
        "trigger": "co2_rise"})
    # Should not have sent another message.
    after_count = len(captured_posts)
    assert after_count == first_count


def test_failed_emergency_enqueue_does_not_mark_sent(relay, captured_posts):
    relay.sender.enqueue = lambda job, block=False: False
    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "distress/state", {
        "id": "distress-enqueue-fail", "severity": "emergency", "state": "active",
        "trigger": "co2_rise"})

    assert captured_posts == []
    assert relay.active_distress_id == "distress-enqueue-fail"
    assert relay.distress_last_sent_at == 0.0


def test_emergency_enqueue_does_not_mark_sent_until_worker_delivers(relay, captured_posts):
    queued_jobs = []
    relay.sender.enqueue = lambda job, block=False: queued_jobs.append(job) or True
    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "distress/state", {
        "id": "distress-worker-success", "severity": "emergency", "state": "active",
        "trigger": "co2_rise"})

    assert queued_jobs
    assert captured_posts == []
    assert relay.active_distress_id == "distress-worker-success"
    assert relay.distress_last_sent_at == 0.0

    relay.sender._process(queued_jobs[0])

    assert len(captured_posts) == 1
    assert relay.distress_last_sent_at > 0.0


def test_unsent_emergency_repeat_is_not_enqueued_twice(relay, captured_posts):
    queued_jobs = []
    relay.sender.enqueue = lambda job, block=False: queued_jobs.append(job) or True
    deliver_to(relay, "state", {"state": "armed"})

    payload = {
        "id": "distress-discord-outage",
        "severity": "emergency",
        "state": "active",
        "trigger": "co2_rise",
    }
    deliver_to(relay, "distress/state", payload)
    deliver_to(relay, "distress/state", payload)

    assert len(queued_jobs) == 1
    assert captured_posts == []
    assert relay.distress_last_sent_at == 0.0


def test_distress_acked_suppresses_repeats(relay, captured_posts):
    """The relay must not re-send @everyone after the firmware reports
    distress_acknowledged=true in bus/state, even when the firmware keeps
    re-publishing bus/distress/state=active at its 60s repeat interval."""
    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "distress/state", {
        "id": "distress-001", "severity": "emergency", "state": "active",
        "trigger": "co2_rise"})
    before_ack_count = len([p for p in captured_posts if "@everyone" in p.json["content"]])
    assert before_ack_count == 1
    # Firmware acks.
    deliver_to(relay, "state", {"state": "distress_active", "distress_acknowledged": True})
    # Firmware re-publishes the same active distress.
    deliver_to(relay, "distress/state", {
        "id": "distress-001", "severity": "emergency", "state": "active",
        "trigger": "co2_rise"})
    after_ack_count = len([p for p in captured_posts if "@everyone" in p.json["content"]])
    assert after_ack_count == 1, "Should not re-send @everyone after ack"


def test_acked_distress_payload_suppresses_send(relay, captured_posts):
    deliver_to(relay, "distress/state", {
        "id": "distress-acked", "severity": "emergency", "state": "acked",
        "trigger": "co2_rise"})
    assert relay.distress_acked is True
    assert captured_posts == []


def test_retained_distress_on_startup_not_resent(relay, captured_posts):
    """If the broker re-delivers a retained distress message on relay
    startup, the relay must not re-send the @everyone. The firmware's
    repeat path will re-publish it as non-retained when appropriate."""
    deliver_to(relay, "distress/state", {
        "id": "distress-001", "severity": "emergency", "state": "active",
        "trigger": "co2_rise"}, retain=True)
    assert all("@everyone" not in p.json["content"] for p in captured_posts)


def test_distress_handles_missing_person_field(relay, captured_posts):
    """CO2 and mmWave triggers omit `person`. The relay must treat it as
    optional and degrade to 'unknown child' rather than crashing."""
    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "distress/state", {
        "id": "distress-002", "severity": "emergency", "state": "active",
        "trigger": "presence_detected",
        "evidence": {"co2": 700},
        "gps": {"valid": False}})
    assert any("unknown child" in p.json["content"] for p in captured_posts)


def test_sensor_only_distress_names_currently_near_beacon(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice_wristband", "person_name": "Alice"}]})
    deliver_to(relay, "state", {"state": "armed"})
    deliver_to(relay, "beacon/alice_wristband/state", {
        "state": "near", "rssi": -68, "timestamp": "2026-07-03T16:10:00Z"})
    captured_posts.clear()

    deliver_to(relay, "distress/state", {
        "id": "distress-presence-001",
        "severity": "emergency",
        "state": "active",
        "trigger": "presence_detected",
        "detail": "presence_detected",
        "timestamp": "2026-07-03T16:12:16Z",
        "evidence": {
            "co2": 1000,
            "co2_delta_since_armed": -6,
            "ld2412_target": True,
            "ld2412_moving": False,
            "ld2412_still": True,
        },
    })

    emergency_post = next(p for p in captured_posts if "@everyone" in p.json["content"])
    assert emergency_post.json["content"] == (
        "@everyone EMERGENCY: possible child left in bus - "
        "Alice (`alice_wristband`). Trigger: Presence detected."
    )
    assert "CO2:" not in emergency_post.json["content"]
    fields = {field["name"]: field["value"] for field in emergency_post.json["embeds"][0]["fields"]}
    assert fields["Child / beacon"] == "Alice (`alice_wristband`)"
    assert fields["Beacon evidence"] == (
        "`alice_wristband`; state near; RSSI -68 dBm; source: currently near beacon state"
    )
    assert fields["CO2"] == "1000 ppm; -6 ppm since armed"
    assert fields["mmWave"] == "target yes; moving no; still yes"


# ===========================================================================
# Fallback behavior
# ===========================================================================
def test_missing_child_channel_falls_back_to_default(relay, captured_posts):
    deliver_to(relay, "beacon/config/state", {"records": [
        {"id": "alice", "person_name": "Alice"}]})  # no discord_channel_id
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "beacon/alice/state", {"state": "near"})
    assert any("222222222222222222" in p.url for p in captured_posts), \
        "Should fall back to default child channel"


# ===========================================================================
# Input validation
# ===========================================================================
def test_invalid_json_logs_and_does_not_crash(relay, captured_posts):
    deliver_to(relay, "state", "{not json")
    deliver_to(relay, "beacon/config/state", {"records": []})  # relay stays healthy
    assert relay.roster == {}


def test_non_dict_payload_does_not_crash(relay, captured_posts):
    deliver_to(relay, "state", "[1,2,3]")  # JSON array, not object
    deliver_to(relay, "beacon/alice/state", '"a string"')
    # Relay should still be functional.
    deliver_to(relay, "state", {"state": "active"})
    deliver_to(relay, "beacon/alice/state", {"state": "near"})
    assert len(captured_posts) >= 1


# ===========================================================================
# Firmware status / degraded
# ===========================================================================
def test_firmware_offline_makes_status_degraded(relay, captured_posts):
    deliver_to(relay, "status", "online")
    assert relay.firmware_online is True
    deliver_to(relay, "status", "offline")
    # status publishes go to the (fake) MQTT; check the last status payload.
    statuses = [json.loads(p[1]) for p in relay.client.published
                if p[0] == "bus/relay/discord/status"]
    assert any(s["status"] == "offline" or s["firmware_online"] is False for s in statuses)


def test_missing_roster_publishes_degraded_status(relay, captured_posts):
    # No roster delivered yet, simulate MQTT connect.
    relay.on_connect(relay.client, None, None, 0, None)
    statuses = [json.loads(p[1]) for p in relay.client.published
                if p[0] == "bus/relay/discord/status"]
    assert any(s["status"] == "degraded" for s in statuses), \
        "Without a roster the relay should be in degraded state"
