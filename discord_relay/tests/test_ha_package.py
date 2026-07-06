"""Lint tests for the Home Assistant package YAML.

These tests don't require HA to be installed; they just confirm the package
file parses and contains the expected structural elements. Real deployment
testing should happen in HA itself.
"""
from pathlib import Path

import pytest

yaml = pytest.importorskip("yaml")

PKG_PATH = str(Path(__file__).resolve().parent.parent.parent / "homeassistant" / "bus_project_package.yaml")
BLUEPRINT_PATH = str(Path(__file__).resolve().parent.parent.parent / "homeassistant" / "bus_child_home_zone_blueprint.yaml")
DASHBOARD_PATH = str(Path(__file__).resolve().parent.parent.parent / "homeassistant" / "dashboard.yaml")

class HomeAssistantLoader(yaml.SafeLoader):
    pass


def _construct_input(loader, node):
    return {"!input": loader.construct_scalar(node)}


HomeAssistantLoader.add_constructor("!input", _construct_input)


def load_ha_yaml(path):
    with open(path, "r", encoding="utf-8") as f:
        return yaml.load(f, Loader=HomeAssistantLoader)


@pytest.fixture(scope="module")
def pkg():
    return load_ha_yaml(PKG_PATH)


@pytest.fixture(scope="module")
def dashboard():
    return load_ha_yaml(DASHBOARD_PATH)


def walk_yaml(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from walk_yaml(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk_yaml(child)


def test_package_has_required_top_level_keys(pkg):
    assert isinstance(pkg, dict)
    for key in ("mqtt", "input_text", "input_number", "input_boolean",
                "script", "automation"):
        assert key in pkg, f"missing top-level key: {key}"


def test_package_has_all_helpers(pkg):
    expected = {
        "bus_beacon_id", "bus_person_name", "bus_ibeacon_uuid",
        "bus_discord_channel_id", "bus_home_zone_entity", "bus_person_notes",
        "bus_device_id",
    }
    found = set(pkg["input_text"].keys())
    assert expected.issubset(found), f"missing helpers: {expected - found}"
    assert "bus_cabin_clear_confirmed" in pkg["input_boolean"]
    assert "bus_exit_grace_reminder_sent" in pkg["input_boolean"]


def test_package_has_add_update_remove_scripts(pkg):
    expected_scripts = {
        "bus_add_person_beacon", "bus_update_person_beacon",
        "bus_remove_person_beacon", "bus_list_person_beacons",
        "bus_acknowledge_distress", "bus_clear_distress",
        "bus_test_distress", "bus_disarm",
    }
    found = set(pkg["script"].keys())
    assert expected_scripts.issubset(found), \
        f"missing scripts: {expected_scripts - found}"


def test_disarm_requires_confirmation_and_waits_for_matching_result(pkg):
    script = pkg["script"]["bus_disarm"]
    rendered = str(script)
    assert "input_boolean.bus_cabin_clear_confirmed" in rendered
    assert "confirm_cabin_clear" in rendered
    for allowed_state in ("active", "exit_grace", "armed", "acked", "disarmed"):
        assert allowed_state in rendered
    assert "sensor.bus_command_result" in rendered
    assert "wait_template" in rendered
    assert "Treat the bus as still armed" in rendered

    result_sensors = [
        sensor for sensor in pkg["mqtt"]["sensor"]
        if sensor.get("unique_id") == "bus_command_result"
    ]
    assert result_sensors == [{
        "name": "Bus Command Result",
        "unique_id": "bus_command_result",
        "state_topic": "bus/command/result",
        "value_template": "{{ value_json.request_id }}",
        "json_attributes_topic": "bus/command/result",
    }]

    expiry = next(
        automation for automation in pkg["automation"]
        if automation.get("id") == "bus_cabin_clear_confirmation_expires"
    )
    assert expiry["trigger"][0]["for"] == "00:02:00"


def test_package_has_school_zone_automations(pkg):
    aliases = {a.get("alias", "") for a in pkg["automation"]}
    assert "Bus School Zone Enter" in aliases
    assert "Bus School Zone Leave" in aliases


def test_package_relays_status_sensor_uses_availability_template(pkg):
    """The relay status topic publishes a JSON object (not the bare word
    'online') so payload_available cannot match. The sensor must use an
    availability_template that parses the JSON."""
    status_sensors = [s for s in pkg["mqtt"]["sensor"]
                      if s.get("unique_id") == "bus_relay_discord_status"]
    assert len(status_sensors) == 1
    s = status_sensors[0]
    assert "availability_template" in s, \
        "relay status sensor must use availability_template (payload_available would never match JSON status)"
    assert "value_json.status" in s["availability_template"]
    assert "offline" in s["availability_template"]


def test_package_emergency_automation_targets_emergency_distress(pkg):
    """The mobile push automation must trigger on MQTT distress with
    severity=emergency and active state."""
    aliases = {a.get("alias", ""): a for a in pkg["automation"]}
    automation = aliases["Bus Distress Mobile Push"]
    assert automation["trigger"] == [{
        "platform": "mqtt",
        "topic": "bus/distress/state",
    }]
    conditions = automation["condition"]
    assert len(conditions) == 1
    template = conditions[0]["value_template"]
    assert "severity == 'emergency'" in template
    assert "['active', 'distress_active']" in template
    actions = automation["action"]
    assert any(action.get("service", "").startswith("notify.") for action in actions)
    rendered_actions = str(actions)
    assert "trigger.payload_json.person.person_name" not in rendered_actions
    assert "trigger.payload_json.gps.valid" not in rendered_actions
    assert "{% set person =" in rendered_actions
    assert "{% set bus_tracker = states.person.bus_tracker %}" in rendered_actions
    assert "state_attr('person.bus_tracker', 'latitude')" in rendered_actions
    assert any(action.get("data", {}).get("data", {}).get("push", {})
               .get("sound", {}).get("critical") == 1 for action in actions)


def test_package_distress_detail_handles_empty_retained_clear(pkg):
    sensors = [s for s in pkg["mqtt"]["sensor"]
               if s.get("unique_id") == "bus_distress_detail"]
    assert len(sensors) == 1
    template = sensors[0]["value_template"]
    assert "value_json is defined" in template
    assert "else 'clear'" in template


def test_exit_grace_reminder_is_state_based_and_latched(pkg):
    aliases = {a.get("alias", ""): a for a in pkg["automation"]}
    reminder = aliases["Bus Exit Grace Reminder"]
    rendered = str(reminder)
    assert "sensor.bus_state_detail" in rendered
    assert "00:01:30" in rendered
    assert "bus_exit_grace_reminder_sent" in rendered
    assert "trigger.payload_json" not in rendered

    reset = aliases["Bus Exit Grace Reminder Resets"]
    reset_rendered = str(reset)
    assert "from': 'exit_grace'" in reset_rendered
    assert "input_boolean.turn_off" in reset_rendered


def test_package_stale_firmware_uses_detail_last_updated(pkg):
    aliases = {a.get("alias", ""): a for a in pkg["automation"]}
    automation = aliases["Bus Stale Firmware Alert"]
    rendered = str(automation)
    assert "sensor.bus_state_detail" in rendered
    assert "last_updated" in rendered
    assert "states.sensor.bus_state.last_changed" not in rendered


def test_blueprint_parses():
    bp = load_ha_yaml(BLUEPRINT_PATH)
    assert bp["blueprint"]["domain"] == "automation"
    assert bp["blueprint"]["input"]["bus_tracker"]["default"] == "person.bus_tracker"
    assert "home_zone" in bp["blueprint"]["input"]
    assert "child_name" in bp["blueprint"]["input"]
    assert bp["trigger"][0]["entity_id"] == {"!input": "bus_tracker"}
    assert bp["trigger"][0]["zone"] == {"!input": "home_zone"}
    # Action must publish zone_enter and zone_leave to bus/ha/location/event.
    actions_yaml = [str(s) for s in bp["action"]]
    assert any("zone_enter" in s for s in actions_yaml)
    assert any("zone_leave" in s for s in actions_yaml)
    assert any("bus/ha/location/event" in s for s in actions_yaml)


def test_dashboard_uses_current_action_syntax(dashboard):
    assert "views" in dashboard
    for node in walk_yaml(dashboard):
        if node.get("action") == "call-service":
            raise AssertionError("dashboard should use perform-action syntax, not legacy call-service")
        if node.get("action") == "perform-action":
            assert "perform_action" in node


def test_dashboard_documents_custom_card_dependencies(dashboard):
    raw = Path(DASHBOARD_PATH).read_text(encoding="utf-8")
    custom_types = {
        node.get("type") for node in walk_yaml(dashboard)
        if isinstance(node.get("type"), str) and node["type"].startswith("custom:")
    }
    assert "custom:mushroom-template-card" in custom_types
    assert "Mushroom" in raw
    assert "auto-entities" in raw
    assert "mini-graph-card" in raw
    assert "timer-bar-card" in raw


def test_dashboard_distress_template_handles_missing_nested_attrs(dashboard):
    rendered = str(dashboard)
    assert "{% set person =" in rendered
    assert "{% set evidence =" in rendered
    assert "person.bus_tracker" in rendered
    assert "state_attr('sensor.bus_distress_detail', 'person').person_name" not in rendered


def test_ha_location_logic_uses_template_tracker(pkg, dashboard):
    assert "person.bus_tracker" in str(dashboard)
    assert "device_tracker.bus_sensor_bus_location" not in str(dashboard)

    assert pkg["template"][0]["sensor"][0]["state"] == "person.bus_tracker"

    zone_automation_ids = {
        "bus_school_zone_enter",
        "bus_school_zone_leave",
        "bus_alice_home_zone_enter",
        "bus_alice_home_zone_leave",
    }
    zone_automations = [
        automation for automation in pkg["automation"]
        if automation.get("id") in zone_automation_ids
    ]
    assert len(zone_automations) == len(zone_automation_ids)
    for automation in zone_automations:
        assert automation["trigger"][0]["entity_id"] == "person.bus_tracker"

    aliases = {automation.get("alias", ""): automation for automation in pkg["automation"]}
    location_publisher = aliases["Bus Publish HA Location State"]
    rendered = str(location_publisher)
    assert "person.bus_tracker" in rendered
    assert "bus/ha/location/state" in rendered
    assert "retain" in rendered
