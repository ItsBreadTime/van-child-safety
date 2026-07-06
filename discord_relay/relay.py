import asyncio
import hashlib
import json
import logging
import os
import queue
import re
import signal
import socket
import sys
import threading
import time
import uuid
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from logging.handlers import RotatingFileHandler
from pathlib import Path

import paho.mqtt.client as mqtt
import requests
import discord
from discord import app_commands


VERSION = "0.5.2"

# Discord channel IDs are 17-20 digit snowflakes.
CHANNEL_ID_RE = re.compile(r"^[0-9]{15,20}$")
# Firmware beacon ids: ^[A-Za-z0-9_-]{1,31}$
BEACON_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,31}$")
IBEACON_UUID_RE = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
)
HOME_ZONE_RE = re.compile(r"^zone\.[A-Za-z0-9_]+$")

# Dedupe key TTL. sent_keys grows without bound without pruning; cap at 24h.
SENT_KEY_TTL_S = 86400


def env(name, default=""):
    return os.environ.get(name, default)


def env_int(name, default):
    try:
        return int(env(name, str(default)))
    except (TypeError, ValueError):
        return default


def env_id_set(name):
    """Parse a comma-separated snowflake allow-list. Invalid values are
    ignored so a typo never broadens command authorization."""
    return {
        item.strip() for item in env(name).split(",")
        if CHANNEL_ID_RE.fullmatch(item.strip())
    }


def now_iso():
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


# ---------------------------------------------------------------------------
# Metrics (Prometheus text format on a small HTTP server)
# ---------------------------------------------------------------------------
class Metrics:
    """Lightweight counter registry. Exposed on /metrics at port 9101 in
    Prometheus text format. The Docker HEALTHCHECK hits /health."""

    def __init__(self):
        self._lock = threading.Lock()
        self._counters = {
            "discord_sends_total": 0,
            "discord_send_failures_total": 0,
            "discord_send_skipped_total": 0,
            "emergency_sends_total": 0,
            "emergency_send_failures_total": 0,
            "emergency_retries_total": 0,
            "distress_received_total": 0,
            "distress_deduped_total": 0,
            "distress_sent_total": 0,
            "mqtt_reconnects_total": 0,
            "mqtt_messages_total": 0,
            "handler_errors_total": 0,
            "invalid_json_total": 0,
        }
        self.start_time = time.time()

    def inc(self, name, n=1):
        with self._lock:
            self._counters[name] = self._counters.get(name, 0) + n

    def render(self):
        with self._lock:
            uptime = int(time.time() - self.start_time)
            lines = ["# HELP bus_relay_uptime_seconds Relay uptime",
                     "# TYPE bus_relay_uptime_seconds counter",
                     f"bus_relay_uptime_seconds {uptime}"]
            for name, value in sorted(self._counters.items()):
                lines.append(f"# HELP {name} cumulative counter")
                lines.append(f"# TYPE {name} counter")
                lines.append(f"{name} {value}")
            return "\n".join(lines) + "\n"


METRICS = Metrics()


class MetricsHTTPHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"ok\n")
        elif self.path == "/metrics":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; version=0.0.4")
            self.end_headers()
            self.wfile.write(METRICS.render().encode("utf-8"))
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *args, **kwargs):
        pass  # silence default stderr access log


def start_metrics_server(port=9101):
    server = ThreadingHTTPServer(("0.0.0.0", port), MetricsHTTPHandler)
    thread = threading.Thread(target=server.serve_forever, name="metrics-http", daemon=True)
    thread.start()
    return server


# ---------------------------------------------------------------------------
# Discord send worker
# ---------------------------------------------------------------------------
#
# Synchronous HTTP inside the paho MQTT callback thread blocks the network
# loop; under load (multiple child transitions, a burst of retries) the loop
# misses MQTT keepalives and disconnects, which re-delivers retained state
# and produces duplicate sends - a live-lock. The send pipeline below runs
# HTTP on a dedicated worker thread; MQTT callbacks push to a bounded queue
# and return immediately. Backpressure is logged, not dropped silently for
# emergency sends (emergency sends block the caller until enqueued).
class SendJob:
    __slots__ = (
        "channel_id", "content", "emergency", "key", "embeds", "enqueued_at",
        "on_success", "on_failure",
    )

    def __init__(self, channel_id, content, emergency, key, embeds=None,
                 on_success=None, on_failure=None):
        self.channel_id = channel_id
        self.content = content
        self.emergency = emergency
        self.key = key
        self.embeds = embeds or []
        self.enqueued_at = time.time()
        self.on_success = on_success
        self.on_failure = on_failure


class DiscordSender(threading.Thread):
    def __init__(self, relay):
        super().__init__(name="discord-sender", daemon=True)
        self.relay = relay
        self.queue: "queue.Queue[SendJob]" = queue.Queue(maxsize=200)
        self.stopping = threading.Event()
        self.discord_ready = bool(relay.discord_token)
        self.last_liveness_at = 0.0

    def enqueue(self, job: SendJob, block: bool):
        try:
            if block:
                self.queue.put(job, timeout=10)
            else:
                self.queue.put_nowait(job)
            return True
        except queue.Full:
            self.relay.log("send_queue_full", emergency=job.emergency, channel_id=job.channel_id)
            return False

    def run(self):
        while not self.stopping.is_set():
            try:
                job = self.queue.get(timeout=1)
            except queue.Empty:
                self._liveness_probe()
                continue
            self._process(job)
            self._liveness_probe()

    def _liveness_probe(self):
        """Periodically GET /users/@me to detect token revocation or network
        partition. Without this, discord_connected stays True forever once a
        token is configured."""
        now = time.time()
        if now - self.last_liveness_at < 60:
            return
        self.last_liveness_at = now
        if not self.relay.discord_token:
            return
        try:
            resp = requests.get(
                "https://discord.com/api/v10/users/@me",
                headers={"Authorization": f"Bot {self.relay.discord_token}",
                         "User-Agent": f"VanChildSafetyDiscordRelay/{VERSION}"},
                timeout=5,
            )
            new_ready = resp.status_code == 200
        except requests.RequestException as exc:
            self.relay.log("discord_liveness_exception", error=str(exc))
            new_ready = False
        if new_ready != self.discord_ready:
            self.relay.log("discord_ready_changed", ready=new_ready)
        self.discord_ready = new_ready

    def stop(self):
        self.stopping.set()

    def _process(self, job: SendJob):
        relay = self.relay

        if not job.channel_id:
            job.channel_id = relay.default_child_channel or relay.global_channel
        if not job.channel_id:
            relay.publish_error("No Discord channel configured for message")
            relay.log("discord_send_skipped", reason="missing_channel", content=job.content)
            METRICS.inc("discord_send_skipped_total")
            self._notify_failure(job)
            return
        if job.key and relay.has_sent_key(job.key):
            relay.log("discord_send_skipped", reason="dedupe", key=job.key)
            METRICS.inc("discord_send_skipped_total")
            self._notify_success(job)
            return
        if not relay.discord_token:
            relay.publish_error("DISCORD_BOT_TOKEN is not configured")
            relay.log("discord_send_skipped", reason="missing_token", channel_id=job.channel_id)
            METRICS.inc("discord_send_skipped_total")
            self._notify_failure(job)
            return
        if not CHANNEL_ID_RE.match(str(job.channel_id)):
            relay.publish_error(f"Invalid Discord channel id: {job.channel_id}")
            relay.log("discord_send_skipped", reason="invalid_channel_id", channel_id=job.channel_id)
            METRICS.inc("discord_send_skipped_total")
            self._notify_failure(job)
            return

        # Emergency sends retry with exponential backoff. A dropped @everyone
        # is a child-safety incident; one attempt is not acceptable.
        if job.emergency:
            METRICS.inc("emergency_sends_total")
            delivered = self._send_with_retry(job)
        else:
            delivered = self._send_once(job)
        if delivered:
            self._notify_success(job)
        else:
            self._notify_failure(job)

    def _notify_success(self, job: SendJob):
        if not job.on_success:
            return
        try:
            job.on_success(job)
        except Exception as exc:
            self.relay.log("discord_send_success_callback_failed", error=str(exc))

    def _notify_failure(self, job: SendJob):
        if not job.on_failure:
            return
        try:
            job.on_failure(job)
        except Exception as exc:
            self.relay.log("discord_send_failure_callback_failed", error=str(exc))

    def _send_once(self, job: SendJob, fallback_attempted: bool = False,
                   rate_limit_attempt: int = 0):
        relay = self.relay
        url = f"https://discord.com/api/v10/channels/{job.channel_id}/messages"
        payload = {
            "content": job.content,
            "allowed_mentions": relay.allowed_mentions_payload(emergency=job.emergency),
        }
        if job.embeds:
            payload["embeds"] = job.embeds[:10]
        headers = {
            "Authorization": f"Bot {relay.discord_token}",
            "Content-Type": "application/json",
            "User-Agent": f"VanChildSafetyDiscordRelay/{VERSION}",
        }
        try:
            resp = requests.post(url, headers=headers, json=payload, timeout=10)
        except requests.RequestException as exc:
            relay.publish_error(f"Discord send exception: {exc}")
            relay.log("discord_send_exception", channel_id=job.channel_id, error=str(exc))
            METRICS.inc("discord_send_failures_total")
            if job.emergency:
                METRICS.inc("emergency_send_failures_total")
            if not fallback_attempted:
                self._maybe_fallback(job)
            return False

        if resp.status_code == 429:
            retry_after = 1.0
            try:
                retry_after = float(resp.json().get("retry_after", 1.0))
            except ValueError:
                pass
            METRICS.inc("discord_send_failures_total")
            if rate_limit_attempt >= 2:
                relay.publish_error("Discord send failed: rate limit retry budget exhausted")
                relay.log("discord_rate_limit_exhausted", channel_id=job.channel_id)
                if job.emergency:
                    METRICS.inc("emergency_send_failures_total")
                if not fallback_attempted:
                    self._maybe_fallback(job)
                return False
            time.sleep(min(max(retry_after, 1.0), 30.0))
            return self._send_once(
                job,
                fallback_attempted=fallback_attempted,
                rate_limit_attempt=rate_limit_attempt + 1,
            )

        if resp.status_code >= 300:
            relay.publish_error(f"Discord send failed: {resp.status_code} {resp.text[:160]}")
            relay.log("discord_send_failed", channel_id=job.channel_id,
                      status=resp.status_code, body=resp.text[:500])
            METRICS.inc("discord_send_failures_total")
            if job.emergency:
                METRICS.inc("emergency_send_failures_total")
            if not fallback_attempted:
                self._maybe_fallback(job)
            return False

        if job.key:
            relay.mark_sent(job.key)
        relay.last_message_at = now_iso()
        METRICS.inc("discord_sends_total")
        relay.publish_json(
            "relay/discord/last_event",
            {"channel_id": job.channel_id, "emergency": job.emergency,
             "content": job.content[:240],
             "embed_titles": [embed.get("title") for embed in job.embeds if embed.get("title")],
             "timestamp": relay.last_message_at},
            retain=True,
        )
        relay.clear_error()
        relay.publish_status()
        relay.log("discord_send_ok", channel_id=job.channel_id,
                  emergency=job.emergency, content=job.content)
        return True

    def _send_with_retry(self, job: SendJob):
        relay = self.relay
        backoffs = [1, 5, 15, 60]
        for attempt, delay in enumerate(backoffs, start=1):
            if self._send_once(job, fallback_attempted=True):
                return True
            if attempt < len(backoffs):
                METRICS.inc("emergency_retries_total")
                relay.log("emergency_retry", attempt=attempt, delay=delay)
                time.sleep(delay)
        relay.publish_error(f"Emergency Discord send failed after {len(backoffs)} attempts")
        relay.log("emergency_send_exhausted", channel_id=job.channel_id)
        return False

    def _maybe_fallback(self, job: SendJob):
        relay = self.relay
        if job.emergency:
            return  # emergency path retries in _send_with_retry
        if not relay.global_channel or job.channel_id == relay.global_channel:
            return
        fb_key = f"fallback:{job.key}" if job.key else None
        fb_job = SendJob(
            relay.global_channel,
            f"Notification fallback: failed to send to <#{job.channel_id}>. Original: {job.content}",
            emergency=False, key=fb_key, embeds=job.embeds,
        )
        self._send_once(fb_job, fallback_attempted=True)


# ---------------------------------------------------------------------------
# Discord operator slash commands
# ---------------------------------------------------------------------------
class DiscordOperatorClient(discord.Client):
    """Minimal gateway client for operator-only application commands.

    Notification delivery remains on the existing REST worker. The gateway is
    used only because Discord interactions are push events and cannot be
    received by the send-only REST path.
    """

    def __init__(self, relay):
        intents = discord.Intents.none()
        intents.guilds = True
        super().__init__(intents=intents)
        self.relay = relay
        self.tree = app_commands.CommandTree(self)
        self.guild = discord.Object(id=int(relay.command_guild_id))
        self.gateway_loop = None

        def role_ids_for(interaction):
            return self.role_ids_for_user(getattr(interaction, "user", None))

        async def reject_if_not_operator(interaction):
            user_id = str(interaction.user.id)
            allowed, reason = relay.validate_discord_operator(user_id, role_ids_for(interaction))
            if allowed:
                return False
            relay.log("discord_command_rejected", user_id=user_id, reason=reason)
            await interaction.response.send_message(
                relay.format_operator_refusal("Command refused", reason),
                ephemeral=True,
            )
            return True

        async def beacon_id_autocomplete(interaction, current: str):
            allowed, _reason = relay.validate_discord_operator(
                str(interaction.user.id), role_ids_for(interaction),
            )
            if not allowed:
                return []
            current_l = (current or "").lower()
            choices = []
            for beacon_id, record in sorted(relay.roster.items()):
                person = record.get("person_name") or beacon_id
                active = "active" if record.get("active", True) else "inactive"
                label = f"{beacon_id} - {person} ({active})"
                if current_l and current_l not in beacon_id.lower() and current_l not in person.lower():
                    continue
                choices.append(app_commands.Choice(name=label[:100], value=beacon_id))
                if len(choices) >= 25:
                    break
            return choices

        @app_commands.command(
            name="disarm",
            description="Disarm after physically confirming the bus cabin is clear",
        )
        @app_commands.describe(
            confirm="Set true only after physically checking the entire cabin",
        )
        async def disarm(interaction: discord.Interaction, confirm: bool):
            await interaction.response.defer(ephemeral=True, thinking=True)
            user_id = str(interaction.user.id)
            role_ids = role_ids_for(interaction)
            allowed, reason = relay.validate_discord_disarm(
                user_id=user_id,
                role_ids=role_ids,
                confirm=confirm,
            )
            if not allowed:
                relay.log("discord_disarm_rejected", user_id=user_id, reason=reason)
                await interaction.edit_original_response(
                    content=relay.format_disarm_response({"ok": False, "reason": reason})
                )
                return

            result = await asyncio.to_thread(
                relay.request_disarm,
                source="discord",
                actor=f"discord_user:{user_id}",
                timeout_s=10,
            )
            await interaction.edit_original_response(
                content=relay.format_disarm_response(result)
            )

        self.tree.add_command(disarm, guild=self.guild)

        @app_commands.command(
            name="bus_status",
            description="Show the current bus, relay, and roster status",
        )
        async def bus_status(interaction: discord.Interaction):
            if await reject_if_not_operator(interaction):
                return
            await interaction.response.send_message(
                relay.format_operator_status(), ephemeral=True,
            )

        self.tree.add_command(bus_status, guild=self.guild)

        beacon = app_commands.Group(
            name="beacon",
            description="Manage child iBeacon roster entries",
        )

        @beacon.command(name="list", description="Show the relay's current child beacon roster")
        async def beacon_list(interaction: discord.Interaction):
            if await reject_if_not_operator(interaction):
                return
            await interaction.response.send_message(
                relay.format_roster_for_discord(), ephemeral=True,
            )

        @beacon.command(name="refresh", description="Ask firmware to republish the roster")
        async def beacon_refresh(interaction: discord.Interaction):
            if await reject_if_not_operator(interaction):
                return
            await interaction.response.defer(ephemeral=True, thinking=True)
            result = await asyncio.to_thread(
                relay.request_beacon_config,
                "list",
                {},
                "discord",
                f"discord_user:{interaction.user.id}",
                10,
            )
            await interaction.edit_original_response(
                content=relay.format_beacon_command_response(result, include_roster=True),
            )

        @beacon.command(name="add", description="Add a child iBeacon roster entry")
        @app_commands.describe(
            id="Short beacon id, 1-31 chars: letters, numbers, underscore, hyphen",
            person_name="Child/person display name",
            uuid="iBeacon UUID in 8-4-4-4-12 hex format",
            major="iBeacon major value, 0-65535",
            minor="iBeacon minor value, 0-65535",
            rssi_threshold="Near threshold in dBm, -127 to 0",
            discord_channel_id="Optional child-specific Discord channel id",
            home_zone_entity="Optional Home Assistant zone entity, e.g. zone.example_home",
            notes="Optional operator notes, max 160 chars",
        )
        async def beacon_add(
            interaction: discord.Interaction,
            id: str,
            person_name: str,
            uuid: str,
            major: int,
            minor: int,
            rssi_threshold: int = -75,
            discord_channel_id: str = "",
            home_zone_entity: str = "",
            notes: str = "",
        ):
            if await reject_if_not_operator(interaction):
                return
            await interaction.response.defer(ephemeral=True, thinking=True)
            payload, error = relay.build_beacon_add_payload(
                id=id,
                person_name=person_name,
                uuid_text=uuid,
                major=major,
                minor=minor,
                rssi_threshold=rssi_threshold,
                discord_channel_id=discord_channel_id,
                home_zone_entity=home_zone_entity,
                notes=notes,
            )
            if error:
                await interaction.edit_original_response(
                    content=relay.format_operator_refusal("Beacon add refused", error)
                )
                return
            result = await asyncio.to_thread(
                relay.request_beacon_config,
                "add",
                payload,
                "discord",
                f"discord_user:{interaction.user.id}",
                10,
            )
            await interaction.edit_original_response(
                content=relay.format_beacon_command_response(result),
            )

        @beacon.command(name="update", description="Update child beacon metadata")
        @app_commands.describe(
            id="Existing beacon id",
            person_name="New display name; leave blank to keep current",
            rssi_threshold="New RSSI threshold -127..0; leave blank to keep current",
            discord_channel_id="New child channel id; leave blank to keep current",
            home_zone_entity="New HA home zone entity; leave blank to keep current",
            notes="New notes; leave blank to keep current",
        )
        async def beacon_update(
            interaction: discord.Interaction,
            id: str,
            person_name: str = "",
            rssi_threshold: str = "",
            discord_channel_id: str = "",
            home_zone_entity: str = "",
            notes: str = "",
        ):
            if await reject_if_not_operator(interaction):
                return
            await interaction.response.defer(ephemeral=True, thinking=True)
            payload, error = relay.build_beacon_update_payload(
                id=id,
                person_name=person_name,
                rssi_threshold=rssi_threshold,
                discord_channel_id=discord_channel_id,
                home_zone_entity=home_zone_entity,
                notes=notes,
            )
            if error:
                await interaction.edit_original_response(
                    content=relay.format_operator_refusal("Beacon update refused", error)
                )
                return
            result = await asyncio.to_thread(
                relay.request_beacon_config,
                "update",
                payload,
                "discord",
                f"discord_user:{interaction.user.id}",
                10,
            )
            await interaction.edit_original_response(
                content=relay.format_beacon_command_response(result),
            )

        beacon_update.autocomplete("id")(beacon_id_autocomplete)

        @beacon.command(name="active", description="Activate or deactivate a child beacon")
        @app_commands.describe(
            id="Existing beacon id",
            active="True to include this beacon in detection; false to disable it",
        )
        async def beacon_active(interaction: discord.Interaction, id: str, active: bool):
            if await reject_if_not_operator(interaction):
                return
            await interaction.response.defer(ephemeral=True, thinking=True)
            payload, error = relay.build_beacon_active_payload(id=id, active=active)
            if error:
                await interaction.edit_original_response(
                    content=relay.format_operator_refusal("Beacon active update refused", error)
                )
                return
            result = await asyncio.to_thread(
                relay.request_beacon_config,
                "update",
                payload,
                "discord",
                f"discord_user:{interaction.user.id}",
                10,
            )
            await interaction.edit_original_response(
                content=relay.format_beacon_command_response(result),
            )

        beacon_active.autocomplete("id")(beacon_id_autocomplete)

        @beacon.command(name="remove", description="Remove a child beacon roster entry")
        @app_commands.describe(
            id="Existing beacon id",
            confirm="Must be true to remove the roster entry",
        )
        async def beacon_remove(interaction: discord.Interaction, id: str, confirm: bool):
            if await reject_if_not_operator(interaction):
                return
            await interaction.response.defer(ephemeral=True, thinking=True)
            payload, error = relay.build_beacon_remove_payload(id=id, confirm=confirm)
            if error:
                await interaction.edit_original_response(
                    content=relay.format_operator_refusal("Beacon remove refused", error)
                )
                return
            result = await asyncio.to_thread(
                relay.request_beacon_config,
                "remove",
                payload,
                "discord",
                f"discord_user:{interaction.user.id}",
                10,
            )
            await interaction.edit_original_response(
                content=relay.format_beacon_command_response(result),
            )

        beacon_remove.autocomplete("id")(beacon_id_autocomplete)

        self.tree.add_command(beacon, guild=self.guild)

    async def setup_hook(self):
        self.gateway_loop = asyncio.get_running_loop()
        synced = await self.tree.sync(guild=self.guild)
        self.relay.log("discord_commands_synced", count=len(synced),
                       guild_id=self.relay.command_guild_id)

    async def on_ready(self):
        self.relay.command_gateway_ready = True
        self.relay.log("discord_gateway_ready", bot_user_id=str(self.user.id))
        self.relay.publish_status()

    async def on_disconnect(self):
        self.relay.command_gateway_ready = False
        self.relay.log("discord_gateway_disconnected")
        self.relay.publish_status()

    def run_blocking(self):
        try:
            self.run(self.relay.discord_token, log_handler=None)
        except Exception as exc:
            self.relay.publish_error(f"Discord command gateway failed: {exc}")
            self.relay.log("discord_gateway_failed", error=str(exc))
        finally:
            self.relay.command_gateway_ready = False
            self.relay.command_client = None
            self.relay.publish_status()

    def stop_threadsafe(self):
        if self.gateway_loop and self.gateway_loop.is_running():
            future = asyncio.run_coroutine_threadsafe(self.close(), self.gateway_loop)
            try:
                future.result(timeout=3)
            except Exception as exc:
                self.relay.log("discord_gateway_stop_failed", error=str(exc))

    @staticmethod
    def role_ids_for_user(user):
        role_ids = set()
        raw_roles = getattr(user, "_roles", None)
        if raw_roles:
            for role_id in raw_roles:
                role_ids.add(str(role_id))
        for role in getattr(user, "roles", []) or []:
            role_id = getattr(role, "id", role)
            role_ids.add(str(role_id))
        return {role_id for role_id in role_ids if CHANNEL_ID_RE.fullmatch(role_id)}


# ---------------------------------------------------------------------------
# Relay
# ---------------------------------------------------------------------------
class DiscordRelay:
    def __init__(self):
        self.base_topic = env("MQTT_BASE_TOPIC", "bus").strip("/")
        self.mqtt_host = env("MQTT_HOST", "127.0.0.1")
        self.mqtt_port = env_int("MQTT_PORT", 1883)
        self.mqtt_username = env("MQTT_USERNAME")
        self.mqtt_password = env("MQTT_PASSWORD")

        self.discord_token = env("DISCORD_BOT_TOKEN")
        self.global_channel = env("DISCORD_GLOBAL_CHANNEL_ID")
        self.default_child_channel = env("DISCORD_DEFAULT_CHILD_CHANNEL_ID")
        self.emergency_channel = env("DISCORD_EMERGENCY_CHANNEL_ID", self.global_channel)
        self.school_zone = env("SCHOOL_ZONE_ENTITY", "zone.school")
        log_path = Path(env("LOG_PATH", "/data/bus-discord-relay.jsonl"))
        self.allowed_mention_raw = env("DISCORD_ALLOWED_MENTION", "@everyone")
        self.distress_repeat_s = env_int("DISTRESS_REPEAT_INTERVAL_S", 300)
        self.client_id = env("MQTT_CLIENT_ID", "bus-discord-relay")
        self.command_guild_id = env("DISCORD_COMMAND_GUILD_ID").strip()
        self.operator_user_ids = env_id_set("DISCORD_OPERATOR_USER_IDS")
        self.operator_role_ids = env_id_set("DISCORD_OPERATOR_ROLE_IDS")

        self.bus_state = {}
        self.telemetry = {}
        self.ha_location = {}
        self.roster = {}
        self.beacon_states = {}
        # Last attendance state that produced a Discord notification. RSSI can
        # chatter near/far around the threshold; only non-RSSI far or absent
        # transitions count as exits.
        self.beacon_attendance_notified = {}
        self.beacon_attendance_pending = {}
        # Pending beacon transitions while bus/state hasn't yet arrived.
        # Race fix: if bus/state=active arrives AFTER bus/beacon/x/state=near,
        # the original implementation dropped the entry notification because
        # bus_allows_attendance() returned False. We hold transitions until
        # bus/state is known, then emit them once state is available.
        self.pending_beacon_transitions = {}
        self.zones_present = set()
        self.zones_present_from_state = set()
        # (key, timestamp) pairs; pruned beyond SENT_KEY_TTL_S.
        self._sent_keys = deque()
        self.active_distress_id = None
        self.distress_last_sent_at = 0.0
        self.distress_last_enqueued_at = {}
        self.distress_received_logged = False
        self.distress_acked = False
        self.last_message_at = None
        self.mqtt_connected = False
        self.firmware_online = False
        self.roster_loaded = False
        self._command_lock = threading.Lock()
        self._command_waiters = {}
        self._beacon_config_waiters = {}
        self.command_client = None
        self.command_thread = None
        self.command_gateway_ready = False

        self._setup_logging(log_path)
        self._setup_mqtt()
        self.sender = DiscordSender(self)
        # Persistent dedupe state: snapshot sent_keys, active_distress_id,
        # and distress_last_sent_at so a relay restart mid-incident doesn't
        # re-send @everyone for an already-known id.
        self._state_path = Path(env("STATE_PATH",
                                    str(log_path.parent / "relay_state.json")))
        self._load_persistent_state()

    def _load_persistent_state(self):
        """Restore dedupe + active distress state from the JSON snapshot.
        On failure (corrupt file, missing), start fresh."""
        try:
            data = json.loads(self._state_path.read_text(encoding="utf-8"))
        except (FileNotFoundError, json.JSONDecodeError, OSError):
            return
        if not isinstance(data, dict):
            return
        restored_keys = data.get("sent_keys", [])
        if isinstance(restored_keys, list):
            for item in restored_keys:
                if not isinstance(item, (list, tuple)) or len(item) != 2:
                    continue
                key, ts = item
                if not isinstance(key, str):
                    continue
                try:
                    self._sent_keys.append((key, float(ts)))
                except (TypeError, ValueError):
                    continue
        self.active_distress_id = data.get("active_distress_id")
        try:
            self.distress_last_sent_at = float(data.get("distress_last_sent_at", 0.0))
        except (TypeError, ValueError):
            self.distress_last_sent_at = 0.0
            self.log("state_restore_field_ignored", field="distress_last_sent_at")
        self._prune_sent_keys()
        self.log("state_restored", state_path=str(self._state_path),
                 sent_keys=len(self._sent_keys),
                 active_distress_id=self.active_distress_id)

    def _save_persistent_state(self):
        """Snapshot dedupe + active distress state. Cheap to call; called
        on every send and on shutdown. Best-effort: any failure is logged."""
        try:
            self._prune_sent_keys()
            data = {
                "sent_keys": list(self._sent_keys),
                "active_distress_id": self.active_distress_id,
                "distress_last_sent_at": self.distress_last_sent_at,
                "saved_at": time.time(),
            }
            tmp = self._state_path.with_suffix(".tmp")
            tmp.write_text(json.dumps(data), encoding="utf-8")
            tmp.replace(self._state_path)
        except OSError as exc:
            self.log("state_save_failed", error=str(exc))

    def _setup_logging(self, log_path: Path):
        self.log_path = log_path
        self.logger = logging.getLogger("relay")
        self.logger.setLevel(logging.INFO)
        # JSONL rotating file handler: prevents unbounded /data growth.
        try:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            file_handler = RotatingFileHandler(
                log_path, maxBytes=5 * 1024 * 1024, backupCount=10, encoding="utf-8",
            )
            file_handler.setFormatter(logging.Formatter("%(message)s"))
            self.logger.addHandler(file_handler)
        except OSError as exc:
            print(f"failed to open log file {log_path}: {exc}", file=sys.stderr)
        stream_handler = logging.StreamHandler()
        stream_handler.setFormatter(logging.Formatter("%(message)s"))
        self.logger.addHandler(stream_handler)

    def _setup_mqtt(self):
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=self.client_id,
            clean_session=False,  # Persist QoS-1 subscriptions so broker queues events during brief disconnects.
        )
        if self.mqtt_username:
            self.client.username_pw_set(self.mqtt_username, self.mqtt_password)
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message

    # ----- status / error / allowed mentions -----
    def topic(self, suffix):
        return f"{self.base_topic}/{suffix}"

    def log(self, event, **fields):
        record = {"timestamp": now_iso(), "event": event, **fields}
        self.logger.info(json.dumps(record, separators=(",", ":"), default=str))

    def publish_json(self, suffix, payload, retain=False):
        try:
            self.client.publish(self.topic(suffix), json.dumps(payload, separators=(",", ":")),
                                 qos=1, retain=retain)
        except Exception as exc:
            self.log("publish_exception", suffix=suffix, error=str(exc))

    def publish_status(self, status="online"):
        # `degraded` when we have no roster or firmware is offline.
        if status == "online" and (not self.roster_loaded or not self.firmware_online):
            status = "degraded"
        self.publish_json(
            "relay/discord/status",
            {
                "status": status,
                "mqtt_connected": self.mqtt_connected,
                "discord_connected": self.sender.discord_ready,
                "firmware_online": self.firmware_online,
                "roster_loaded": self.roster_loaded,
                "roster_count": len(self.roster),
                "bus_state": (self.bus_state.get("state") if isinstance(self.bus_state, dict) else None),
                "active_distress_id": self.active_distress_id,
                "operator_commands_enabled": self.operator_commands_enabled(),
                "operator_gateway_ready": self.command_gateway_ready,
                "version": VERSION,
                "timestamp": now_iso(),
            },
            retain=True,
        )

    def publish_error(self, message):
        self.publish_json(
            "relay/discord/error",
            {"status": "error", "message": message, "timestamp": now_iso()},
            retain=True,
        )

    def clear_error(self):
        try:
            self.publish_json(
                "relay/discord/error",
                {"status": "clear", "message": "clear", "timestamp": now_iso()},
                retain=True,
            )
        except Exception as exc:
            self.log("clear_error_exception", error=str(exc))

    def allowed_mentions_payload(self, emergency):
        """Map DISCORD_ALLOWED_MENTION env to a Discord allowed_mentions object.
        Normal messages suppress all mentions; only emergency messages honor
        the configured mention."""
        if not emergency:
            return {"parse": []}
        raw = self.allowed_mention_raw.strip()
        # @everyone / @here -> parse list
        if raw in ("@everyone", "everyone"):
            return {"parse": ["everyone"]}
        if raw in ("@here", "here"):
            return {"parse": ["here"]}
        # <@&ROLE_ID> -> roles list
        m = re.match(r"^<@&(\d+)>$", raw)
        if m:
            return {"roles": [m.group(1)]}
        # Fallback: everyone for emergency (safety over quiet).
        return {"parse": ["everyone"]}

    # ----- dedupe -----
    def has_sent_key(self, key):
        self._prune_sent_keys()
        return any(k == key for k, _ in self._sent_keys)

    def mark_sent(self, key):
        if not key:
            return
        ts = time.time()
        # Avoid duplicate entries for the same key.
        if not any(k == key for k, _ in self._sent_keys):
            self._sent_keys.append((key, ts))
        self._prune_sent_keys()
        # Persist asynchronously: state writes are cheap but not free; for
        # every-message is acceptable because we throttle with the TTL.
        self._save_persistent_state()

    def _prune_sent_keys(self):
        cutoff = time.time() - SENT_KEY_TTL_S
        while self._sent_keys and self._sent_keys[0][1] < cutoff:
            self._sent_keys.popleft()

    def has_been_sent_recently(self, channel_id, content, window_s=300):
        """Best-effort intra-session dedupe by (channel, content hash, ts window).
        Used for emergency sends that don't have a stable dedupe key."""
        # Walk recent sent keys. Not used today, reserved for future.
        return False

    # ----- Discord send entrypoints -----
    def send_discord(self, channel_id, content, emergency=False, key=None, embeds=None,
                     on_success=None, on_failure=None):
        """Queue a message for the sender thread. Returns True if enqueued.
        Emergency sends block (with timeout) to guarantee enqueuing."""
        if key and self.has_sent_key(key):
            self.log("discord_send_skipped", reason="dedupe_pre_enqueue", key=key)
            METRICS.inc("discord_send_skipped_total")
            return False
        job = SendJob(channel_id, content, emergency, key, embeds=embeds,
                      on_success=on_success, on_failure=on_failure)
        return self.sender.enqueue(job, block=emergency)

    # ----- Discord message formatting -----
    @staticmethod
    def _truncate(value, limit=1024):
        text = "" if value is None else str(value)
        if len(text) <= limit:
            return text
        return text[:limit - 3] + "..."

    def make_embed(self, title, description="", color=0x3498DB, fields=None):
        embed = {
            "title": self._truncate(title, 256),
            "color": color,
            "timestamp": now_iso(),
        }
        if description:
            embed["description"] = self._truncate(description, 4096)
        out_fields = []
        for field in fields or []:
            if len(field) == 2:
                name, value = field
                inline = True
            else:
                name, value, inline = field
            if value is None or value == "":
                continue
            out_fields.append({
                "name": self._truncate(name, 256),
                "value": self._truncate(value, 1024),
                "inline": bool(inline),
            })
            if len(out_fields) >= 25:
                break
        if out_fields:
            embed["fields"] = out_fields
        return embed

    def beacon_event_embed(self, title, beacon_id, data=None, color=0x3498DB,
                           extra_fields=None):
        data = data or {}
        record = self.roster.get(beacon_id, {})
        bus_state = (self.bus_state.get("state") if isinstance(self.bus_state, dict)
                     else self.telemetry.get("bus_state"))
        fields = [
            ("Child", self.child_name(beacon_id)),
            ("Beacon ID", f"`{beacon_id}`"),
            ("Beacon state", data.get("state") or self.beacon_states.get(beacon_id, {}).get("state")),
            ("RSSI", data.get("rssi")),
            ("RSSI threshold", data.get("rssi_threshold") or record.get("rssi_threshold")),
            ("Bus state", bus_state or "unknown"),
            ("Home zone", record.get("home_zone_entity")),
            ("Notes", record.get("notes"), False),
        ]
        if extra_fields:
            fields.extend(extra_fields)
        return self.make_embed(title, color=color, fields=fields)

    def bus_event_embed(self, title, description="", color=0x3498DB, extra_fields=None):
        state = self.bus_state.get("state") if isinstance(self.bus_state, dict) else None
        fields = [
            ("Bus state", state or "unknown"),
            ("Firmware", "online" if self.firmware_online else "offline"),
            ("MQTT", "online" if self.mqtt_connected else "offline"),
        ]
        if extra_fields:
            fields.extend(extra_fields)
        return self.make_embed(title, description=description, color=color, fields=fields)

    # ----- guarded operator commands -----
    def validate_discord_operator(self, user_id, role_ids):
        if not self.operator_user_ids and not self.operator_role_ids:
            return False, "operator allow-list is not configured"
        if (user_id not in self.operator_user_ids and
                not (set(role_ids) & self.operator_role_ids)):
            return False, "you are not an authorized bus operator"
        return True, "authorized"

    def validate_discord_disarm(self, user_id, role_ids, confirm):
        if not confirm:
            return False, "cabin-clear confirmation is required"
        allowed, reason = self.validate_discord_operator(user_id, role_ids)
        if not allowed:
            return False, reason
        if not self.mqtt_connected:
            return False, "MQTT is offline"
        if not self.firmware_online:
            return False, "firmware is offline"
        state = self.bus_state.get("state") if isinstance(self.bus_state, dict) else None
        if state in ("distress_pending", "distress_active"):
            return False, "acknowledge the active distress before disarming"
        if state not in ("active", "exit_grace", "armed", "acked", "disarmed"):
            return False, f"firmware state {state or 'unknown'} is not safe to disarm"
        return True, "authorized"

    def operator_commands_enabled(self):
        return bool(
            self.command_client and
            self.command_gateway_ready and
            self.command_thread and
            self.command_thread.is_alive()
        )

    def request_disarm(self, source, actor, timeout_s=10):
        """Publish a correlated disarm request and block for firmware result.

        Called from asyncio.to_thread for Discord, so waiting never blocks the
        Discord gateway or paho network loops.
        """
        if not self.mqtt_connected:
            return {"ok": False, "reason": "mqtt_offline"}
        if not self.firmware_online:
            return {"ok": False, "reason": "firmware_offline"}

        request_id = f"{source}-disarm-{uuid.uuid4().hex[:16]}"
        waiter = {"event": threading.Event(), "result": None}
        with self._command_lock:
            self._command_waiters[request_id] = waiter

        payload = {
            "request_id": request_id,
            "action": "disarm",
            "confirm_cabin_clear": True,
            "source": source,
            "actor": actor,
        }
        try:
            info = self.client.publish(
                self.topic("command"),
                json.dumps(payload, separators=(",", ":")),
                qos=1,
                retain=False,
            )
            if hasattr(info, "wait_for_publish"):
                info.wait_for_publish(timeout=3)
            if not waiter["event"].wait(timeout_s):
                self.log("disarm_result_timeout", request_id=request_id, source=source)
                return {"ok": False, "reason": "firmware_confirmation_timeout",
                        "request_id": request_id}
            result = waiter["result"] or {
                "ok": False, "reason": "empty_firmware_result", "request_id": request_id,
            }
            if result.get("ok") and not self._wait_for_disarmed_state(timeout_s=min(2.0, timeout_s)):
                self.log("disarm_state_confirmation_timeout", request_id=request_id,
                         result_state=result.get("state"),
                         bus_state=(self.bus_state.get("state")
                                    if isinstance(self.bus_state, dict) else None))
                return {
                    "ok": False,
                    "reason": "bus_state_confirmation_timeout",
                    "request_id": request_id,
                    "firmware_result": result,
                }
            return result
        except Exception as exc:
            self.log("disarm_publish_failed", request_id=request_id, error=str(exc))
            return {"ok": False, "reason": "mqtt_publish_failed",
                    "request_id": request_id}
        finally:
            with self._command_lock:
                self._command_waiters.pop(request_id, None)

    def _wait_for_disarmed_state(self, timeout_s=2.0):
        deadline = time.time() + max(0.0, timeout_s)
        while True:
            state = (self.bus_state.get("state") if isinstance(self.bus_state, dict) else None)
            if state == "disarmed":
                return True
            if time.time() >= deadline:
                return False
            time.sleep(0.05)

    def handle_command_result(self, data):
        if not isinstance(data, dict) or data.get("action") != "disarm":
            return
        request_id = data.get("request_id")
        with self._command_lock:
            waiter = self._command_waiters.get(request_id)
            if waiter:
                waiter["result"] = data
                waiter["event"].set()
        self.log("disarm_result", request_id=request_id, ok=bool(data.get("ok")),
                 reason=data.get("reason"), source=data.get("source"))

    def _clean_text(self, value):
        return str(value).strip() if value is not None else ""

    def _validate_beacon_id(self, beacon_id):
        beacon_id = self._clean_text(beacon_id)
        if not BEACON_ID_RE.fullmatch(beacon_id):
            return None, "id must be 1-31 chars of A-Z, a-z, 0-9, underscore, or hyphen"
        return beacon_id, None

    def _validate_channel_id(self, channel_id, field="discord_channel_id"):
        channel_id = self._clean_text(channel_id)
        if channel_id and not CHANNEL_ID_RE.fullmatch(channel_id):
            return None, f"{field} must be a Discord numeric channel id"
        return channel_id, None

    def _validate_home_zone(self, home_zone_entity):
        home_zone_entity = self._clean_text(home_zone_entity)
        if home_zone_entity and not HOME_ZONE_RE.fullmatch(home_zone_entity):
            return None, "home_zone_entity must look like zone.child_home"
        return home_zone_entity, None

    def _validate_rssi(self, value):
        try:
            rssi = int(value)
        except (TypeError, ValueError):
            return None, "rssi_threshold must be an integer"
        if rssi < -127 or rssi > 0:
            return None, "rssi_threshold must be between -127 and 0"
        return rssi, None

    def build_beacon_add_payload(self, id, person_name, uuid_text, major, minor,
                                 rssi_threshold=-75, discord_channel_id="",
                                 home_zone_entity="", notes=""):
        beacon_id, error = self._validate_beacon_id(id)
        if error:
            return None, error
        person_name = self._clean_text(person_name)
        if not person_name or len(person_name.encode("utf-8")) > 48:
            return None, "person_name must be 1-48 bytes"
        uuid_text = self._clean_text(uuid_text)
        if not IBEACON_UUID_RE.fullmatch(uuid_text):
            return None, "uuid must be 8-4-4-4-12 hex format"
        if major < 0 or major > 65535 or minor < 0 or minor > 65535:
            return None, "major and minor must be 0-65535"
        rssi_threshold, error = self._validate_rssi(rssi_threshold)
        if error:
            return None, error
        discord_channel_id, error = self._validate_channel_id(discord_channel_id)
        if error:
            return None, error
        home_zone_entity, error = self._validate_home_zone(home_zone_entity)
        if error:
            return None, error
        notes = self._clean_text(notes)
        if len(notes.encode("utf-8")) > 160:
            return None, "notes must be at most 160 bytes"
        return {
            "id": beacon_id,
            "person_name": person_name,
            "type": "ibeacon",
            "uuid": uuid_text,
            "major": int(major),
            "minor": int(minor),
            "rssi_threshold": rssi_threshold,
            "discord_channel_id": discord_channel_id,
            "home_zone_entity": home_zone_entity,
            "notes": notes,
        }, None

    def build_beacon_update_payload(self, id, person_name="", rssi_threshold="",
                                    discord_channel_id="", home_zone_entity="",
                                    notes=""):
        beacon_id, error = self._validate_beacon_id(id)
        if error:
            return None, error
        payload = {"id": beacon_id}
        person_name = self._clean_text(person_name)
        if person_name:
            if len(person_name.encode("utf-8")) > 48:
                return None, "person_name must be 1-48 bytes"
            payload["person_name"] = person_name
        rssi_threshold = self._clean_text(rssi_threshold)
        if rssi_threshold:
            rssi, error = self._validate_rssi(rssi_threshold)
            if error:
                return None, error
            payload["rssi_threshold"] = rssi
        discord_channel_id = self._clean_text(discord_channel_id)
        if discord_channel_id:
            discord_channel_id, error = self._validate_channel_id(discord_channel_id)
            if error:
                return None, error
            payload["discord_channel_id"] = discord_channel_id
        home_zone_entity = self._clean_text(home_zone_entity)
        if home_zone_entity:
            home_zone_entity, error = self._validate_home_zone(home_zone_entity)
            if error:
                return None, error
            payload["home_zone_entity"] = home_zone_entity
        notes = self._clean_text(notes)
        if notes:
            if len(notes.encode("utf-8")) > 160:
                return None, "notes must be at most 160 bytes"
            payload["notes"] = notes
        if len(payload) == 1:
            return None, "at least one field must be provided"
        return payload, None

    def build_beacon_active_payload(self, id, active):
        beacon_id, error = self._validate_beacon_id(id)
        if error:
            return None, error
        return {"id": beacon_id, "active": bool(active)}, None

    def build_beacon_remove_payload(self, id, confirm):
        if not confirm:
            return None, "confirm must be true"
        beacon_id, error = self._validate_beacon_id(id)
        if error:
            return None, error
        return {"id": beacon_id}, None

    def validate_beacon_config_safety(self, action, payload):
        payload = payload or {}
        beacon_id = payload.get("id")
        destructive = action == "remove" or (
            action == "update" and payload.get("active") is False
        )
        if not destructive:
            return None
        state = self.bus_state.get("state") if isinstance(self.bus_state, dict) else None
        if state in ("armed", "distress_pending", "distress_active"):
            return (
                "destructive beacon roster changes are blocked while bus state "
                f"is {state}"
            )
        if beacon_id and self.beacon_states.get(beacon_id, {}).get("state") == "near":
            return f"destructive beacon roster changes are blocked while {beacon_id} is near"
        return None

    def request_beacon_config(self, action, payload=None, source="discord",
                              actor="", timeout_s=10):
        if not self.mqtt_connected:
            return {"ok": False, "reason": "mqtt_offline", "action": action}
        if not self.firmware_online:
            return {"ok": False, "reason": "firmware_offline", "action": action}
        if action not in ("add", "update", "remove", "list"):
            return {"ok": False, "reason": "invalid_action", "action": action}
        safety_error = self.validate_beacon_config_safety(action, payload)
        if safety_error:
            beacon_id = (payload or {}).get("id")
            self.log("beacon_config_safety_refused", action=action,
                     beacon_id=beacon_id, reason=safety_error)
            return {
                "ok": False,
                "reason": "safety_state_blocks_roster_change",
                "message": safety_error,
                "action": action,
                "id": beacon_id,
            }

        request_id = f"{source}-beacon-{action}-{uuid.uuid4().hex[:16]}"
        waiter = {"event": threading.Event(), "result": None}
        with self._command_lock:
            self._beacon_config_waiters[request_id] = waiter

        body = {
            "request_id": request_id,
            "action": action,
            "source": source,
            "actor": actor,
        }
        if payload:
            body.update(payload)
        try:
            info = self.client.publish(
                self.topic("beacon/config/set"),
                json.dumps(body, separators=(",", ":")),
                qos=1,
                retain=False,
            )
            if hasattr(info, "wait_for_publish"):
                info.wait_for_publish(timeout=3)
            if not waiter["event"].wait(timeout_s):
                self.log("beacon_config_result_timeout", request_id=request_id,
                         action=action, source=source)
                return {"ok": False, "reason": "firmware_confirmation_timeout",
                        "request_id": request_id, "action": action}
            return waiter["result"] or {
                "ok": False, "reason": "empty_firmware_result",
                "request_id": request_id, "action": action,
            }
        except Exception as exc:
            self.log("beacon_config_publish_failed", request_id=request_id,
                     action=action, error=str(exc))
            return {"ok": False, "reason": "mqtt_publish_failed",
                    "request_id": request_id, "action": action}
        finally:
            with self._command_lock:
                self._beacon_config_waiters.pop(request_id, None)

    def handle_beacon_config_result(self, data):
        if not isinstance(data, dict):
            return
        request_id = data.get("request_id")
        with self._command_lock:
            waiter = self._beacon_config_waiters.get(request_id)
            if waiter:
                waiter["result"] = data
                waiter["event"].set()
        self.log("beacon_config_result", request_id=request_id,
                 action=data.get("action"), ok=bool(data.get("ok")),
                 id=data.get("id"), error=data.get("error"),
                 message=data.get("message"))

    @staticmethod
    def humanize_token(value):
        text = str(value or "unknown").replace("_", " ").replace("-", " ").strip()
        return text[:1].upper() + text[1:] if text else "Unknown"

    @staticmethod
    def format_bool(value):
        if value is True:
            return "yes"
        if value is False:
            return "no"
        return None

    @staticmethod
    def format_number(value, unit="", precision=0):
        if value is None:
            return None
        if isinstance(value, bool):
            return "yes" if value else "no"
        try:
            number = float(value)
        except (TypeError, ValueError):
            return str(value)
        if precision <= 0 and number.is_integer():
            rendered = str(int(number))
        else:
            rendered = f"{number:.{precision}f}"
        return f"{rendered} {unit}".strip()

    def child_display(self, beacon_id=None, person_name=None):
        beacon_id = self._clean_text(beacon_id)
        person_name = (
            self._clean_text(person_name)
            or (self.child_name(beacon_id) if beacon_id else "")
        )
        if beacon_id and person_name and person_name != beacon_id:
            return f"{person_name} (`{beacon_id}`)"
        if beacon_id:
            return f"`{beacon_id}`"
        if person_name:
            return person_name
        return "unknown child"

    def active_near_children(self):
        children = []
        for beacon_id, state in sorted(self.beacon_states.items()):
            if state.get("state") == "near" and self.child_is_active(beacon_id):
                children.append({
                    "id": beacon_id,
                    "name": self.child_name(beacon_id),
                    "rssi": state.get("rssi"),
                    "state": state,
                })
        return children

    def distress_child_context(self, data):
        person = data.get("person") if isinstance(data.get("person"), dict) else {}
        beacon_id = (person.get("id") or person.get("beacon_id")
                     or data.get("beacon_id"))
        person_name = person.get("person_name")

        detail = self._clean_text(data.get("detail"))
        trigger = self._clean_text(data.get("trigger"))
        if (not beacon_id and detail and detail != trigger
                and BEACON_ID_RE.fullmatch(detail)):
            beacon_id = detail
        if beacon_id:
            return {
                "summary": self.child_display(beacon_id, person_name),
                "beacon_ids": [beacon_id],
                "source": "distress payload",
            }

        near_children = self.active_near_children()
        if near_children:
            return {
                "summary": ", ".join(
                    self.child_display(child["id"], child["name"])
                    for child in near_children
                ),
                "beacon_ids": [child["id"] for child in near_children],
                "source": "currently near beacon state",
            }

        return {
            "summary": "unknown child (no active beacon currently near)",
            "beacon_ids": [],
            "source": "sensor-only distress",
        }

    def format_operator_refusal(self, title, reason):
        state = self.bus_state.get("state") if isinstance(self.bus_state, dict) else None
        return (
            f"{title}: {reason}.\n"
            f"Bus state: {state or 'unknown'}.\n"
            "No command was sent to firmware."
        )

    def format_disarm_response(self, result):
        state = self.bus_state.get("state") if isinstance(self.bus_state, dict) else None
        reason = (result.get("message") or result.get("reason") or "no firmware confirmation"
                  if isinstance(result, dict) else "no firmware confirmation")
        request_id = result.get("request_id") if isinstance(result, dict) else None
        if isinstance(result, dict) and result.get("ok"):
            lines = [
                f"Bus disarmed: {reason}.",
                f"Bus state: {state or result.get('state') or 'unknown'}.",
                "Detection resumes on the next ignition cycle.",
            ]
        else:
            lines = [
                f"Disarm failed: {reason}.",
                f"Bus state: {state or 'unknown'}.",
                "Treat the bus as still armed until firmware reports `disarmed`.",
            ]
        if request_id:
            lines.append(f"Request: `{request_id}`")
        return "\n".join(lines)

    def format_operator_status(self):
        state = self.bus_state.get("state") if isinstance(self.bus_state, dict) else "unknown"
        active_distress = self.active_distress_id or "none"
        near_children = self.active_near_children()
        near_text = ", ".join(
            self.child_display(child["id"], child["name"]) for child in near_children
        ) or "none"
        lines = [
            "VanChildSafety relay status",
            f"- MQTT: {'online' if self.mqtt_connected else 'offline'}",
            f"- Firmware: {'online' if self.firmware_online else 'offline'}",
            f"- Discord API: {'online' if self.sender.discord_ready else 'offline'}",
            f"- Bus state: {state or 'unknown'}",
            f"- Roster: {'loaded' if self.roster_loaded else 'not loaded'} ({len(self.roster)} records)",
            f"- Children currently near: {near_text}",
            f"- Active distress: {active_distress}",
            f"- Last Discord message: {self.last_message_at or 'none'}",
            f"- Version: {VERSION}",
        ]
        return "\n".join(lines)

    def format_roster_for_discord(self):
        if not self.roster_loaded:
            return "Roster is not loaded yet. Use `/beacon refresh` after MQTT and firmware are online."
        if not self.roster:
            return "No child beacons are currently registered."
        lines = [f"Registered child beacons ({len(self.roster)}):"]
        for beacon_id, record in sorted(self.roster.items()):
            active = "active" if record.get("active", True) else "inactive"
            name = record.get("person_name") or beacon_id
            channel = record.get("discord_channel_id") or self.default_child_channel or "default/global"
            if CHANNEL_ID_RE.fullmatch(str(channel)):
                channel = f"<#{channel}>"
            home = record.get("home_zone_entity") or "no home zone"
            state = self.beacon_states.get(beacon_id, {}).get("state", "unknown")
            lines.append(f"- `{beacon_id}`: {name} [{active}], state={state}, channel={channel}, home={home}")
        text = "\n".join(lines)
        if len(text) > 1900:
            return text[:1880] + "\n... roster truncated for Discord."
        return text

    def format_beacon_command_response(self, result, include_roster=False):
        ok = bool(result.get("ok")) if isinstance(result, dict) else False
        action = result.get("action", "beacon config") if isinstance(result, dict) else "beacon config"
        if ok:
            message = result.get("message") or "firmware accepted the command"
            changed_id = result.get("id")
            header = f"Beacon {action} succeeded: {message}"
            if changed_id:
                header += f" (`{changed_id}`)"
        else:
            reason = (result.get("message") or result.get("reason") or result.get("error")
                      if isinstance(result, dict) else "unknown")
            header = f"Beacon {action} failed: {reason}"
        request_id = result.get("request_id") if isinstance(result, dict) else None
        if request_id:
            header += f"\nRequest: `{request_id}`"
        if include_roster:
            return f"{header}\n\n{self.format_roster_for_discord()}"
        return header

    # ----- MQTT callbacks -----
    @staticmethod
    def _mqtt_connack_succeeded(reason_code):
        if reason_code == 0:
            return True
        value = getattr(reason_code, "value", None)
        if value == 0:
            return True
        return str(reason_code).lower() in ("0", "success")

    def on_connect(self, client, userdata, flags, reason_code, properties):
        if not self._mqtt_connack_succeeded(reason_code):
            self.mqtt_connected = False
            self.log("mqtt_connect_rejected", reason_code=str(reason_code))
            return
        self.mqtt_connected = True
        METRICS.inc("mqtt_reconnects_total")
        self.log("mqtt_connected", reason_code=str(reason_code))
        for suffix in (
            "status",
            "state",
            "telemetry",
            "beacon/+/state",
            "beacon/config/state",
            "beacon/config/result",
            "distress/state",
            "event",
            "ha/location/event",
            "ha/location/state",
            "command/result",
        ):
            client.subscribe(self.topic(suffix), qos=1)
        self.publish_status("online")

    def on_disconnect(self, client, userdata, flags, reason_code, properties):
        self.mqtt_connected = False
        self.log("mqtt_disconnected", reason_code=str(reason_code))

    def parse_payload(self, message):
        if message.payload is None:
            return None
        try:
            payload = message.payload.decode("utf-8")
        except UnicodeDecodeError:
            payload = message.payload.decode("utf-8", errors="replace")
        if payload == "":
            return None
        try:
            return json.loads(payload)
        except json.JSONDecodeError:
            self.log("invalid_json", topic=message.topic, payload=payload[:240])
            METRICS.inc("invalid_json_total")
            return None

    def parse_text_payload(self, message):
        if message.payload is None:
            return None
        try:
            return message.payload.decode("utf-8").strip()
        except UnicodeDecodeError:
            return message.payload.decode("utf-8", errors="replace").strip()

    def on_message(self, client, userdata, message):
        METRICS.inc("mqtt_messages_total")
        # Wrap every dispatch: an uncaught exception propagates into the paho
        # loop, leaving state inconsistent and losing the in-flight message.
        try:
            suffix = (message.topic[len(self.base_topic) + 1:]
                      if message.topic.startswith(self.base_topic + "/") else message.topic)
            if suffix == "status":
                data = self.parse_text_payload(message)
                self.handle_firmware_status(data, retained=message.retain)
                return

            data = self.parse_payload(message)

            if suffix == "state":
                self.handle_bus_state(data, retained=message.retain)
            elif suffix == "telemetry":
                if isinstance(data, dict):
                    self.telemetry = data
            elif suffix == "beacon/config/state" and data is not None:
                if isinstance(data, dict):
                    self.handle_roster(data)
            elif suffix == "beacon/config/result" and data is not None:
                if isinstance(data, dict):
                    self.handle_beacon_config_result(data)
            elif suffix.startswith("beacon/") and suffix.endswith("/state"):
                beacon_id = suffix.split("/")[1]
                self.handle_beacon_state(beacon_id, data, retained=message.retain)
            elif suffix == "distress/state":
                self.handle_distress(data, retained=message.retain)
            elif suffix == "event" and data is not None:
                if isinstance(data, dict):
                    self.handle_bus_event(data, retained=message.retain)
            elif suffix == "ha/location/event" and data is not None:
                if isinstance(data, dict):
                    self.handle_location_event(data, retained=message.retain)
            elif suffix == "ha/location/state" and data is not None:
                if isinstance(data, dict):
                    self.handle_ha_location_state(data)
            elif suffix == "command/result" and data is not None:
                self.handle_command_result(data)
        except Exception as exc:
            METRICS.inc("handler_errors_total")
            self.log("handler_error", topic=message.topic, error=str(exc))

    # ----- state handlers -----
    def handle_firmware_status(self, data, retained=False):
        if data == "online":
            self.firmware_online = True
            self.log("firmware_online")
        elif data == "offline":
            self.firmware_online = False
            self.log("firmware_offline")
            # LWT: retained, but the firmware reconnects fast typically.
        self.publish_status()

    def handle_bus_state(self, data, retained=False):
        if not isinstance(data, dict):
            return
        old_state = (self.bus_state.get("state") if isinstance(self.bus_state, dict) else None)
        acknowledged_before = (self.bus_state.get("distress_acknowledged")
                                if isinstance(self.bus_state, dict) else False)
        self.bus_state = data
        new_state = data.get("state")
        acknowledged_now = data.get("distress_acknowledged")

        # Detect ack transition to suppress distress repeats (firmware keeps
        # re-publishing distress.state=active after ack at its 60s interval;
        # the relay must stop re-sending @everyone once acked).
        if acknowledged_now and not acknowledged_before:
            self.distress_acked = True
            self.log("distress_acked", distress_id=self.active_distress_id)

        # A pending-bus/state arrival may release pending beacon transitions.
        # The late-state race fix: transitions queued while bus/state was unknown.
        if retained:
            return
        if old_state != new_state and new_state in ("active", "exit_grace"):
            self._flush_pending_beacon_transitions()

    def handle_roster(self, data):
        records = data.get("records") or data.get("beacons") or []
        if not isinstance(records, list):
            return
        previous_ids = set(self.roster.keys())
        roster = {}
        for record in records:
            if not isinstance(record, dict):
                continue
            beacon_id = record.get("id") or record.get("name")
            if not beacon_id or not BEACON_ID_RE.match(str(beacon_id)):
                continue
            roster[beacon_id] = {
                "id": beacon_id,
                "person_name": record.get("person_name") or beacon_id,
                "discord_channel_id": (record.get("discord_channel_id")
                                       or (record.get("discord") or {}).get("channel_id") or ""),
                "home_zone_entity": (record.get("home_zone_entity")
                                     or (record.get("home") or {}).get("zone_entity") or ""),
                "active": record.get("active", True),
                "notes": record.get("notes", ""),
            }
        self.roster = roster
        self.roster_loaded = True
        inactive_ids = {bid for bid, record in roster.items()
                        if record.get("active") is False}
        pruned_ids = (previous_ids - set(roster.keys())) | inactive_ids
        for beacon_id in pruned_ids:
            self.beacon_states.pop(beacon_id, None)
            self.beacon_attendance_notified.pop(beacon_id, None)
            self.beacon_attendance_pending.pop(beacon_id, None)
            self.pending_beacon_transitions.pop(beacon_id, None)
        self.log("roster_update", count=len(roster),
                 beacon_ids=list(roster.keys()),
                 pruned_beacon_ids=sorted(pruned_ids))
        self.publish_status()

    def child_channel(self, beacon_id):
        record = self.roster.get(beacon_id, {})
        return record.get("discord_channel_id") or self.default_child_channel

    def child_name(self, beacon_id):
        return (self.roster.get(beacon_id, {}).get("person_name")
                or self.beacon_states.get(beacon_id, {}).get("person_name")
                or beacon_id)

    def child_is_active(self, beacon_id):
        if not self.roster_loaded:
            return True
        record = self.roster.get(beacon_id)
        return bool(record and record.get("active", True))

    def bus_allows_attendance(self):
        state = (self.bus_state.get("state") if isinstance(self.bus_state, dict)
                 else self.telemetry.get("bus_state"))
        return state in ("active", "exit_grace")

    def bus_allows_attendance_or_unknown(self):
        """For the late-state race: if bus/state is unknown, queue the
        transition. If known-and-allowing, fire immediately."""
        state = (self.bus_state.get("state") if isinstance(self.bus_state, dict)
                 else self.telemetry.get("bus_state"))
        if state is None:
            return None  # unknown -> queue
        return state in ("active", "exit_grace")

    def in_zone(self, zone_entity):
        return bool(zone_entity and (
            zone_entity in self.zones_present
            or zone_entity in self.zones_present_from_state
        ))

    def handle_beacon_state(self, beacon_id, data, retained=False):
        if not BEACON_ID_RE.match(beacon_id):
            self.log("invalid_beacon_id_in_topic", beacon_id=beacon_id)
            return
        if not data:
            if not isinstance(data, dict):
                self.beacon_states.pop(beacon_id, None)
            return
        if not isinstance(data, dict):
            return

        old = self.beacon_states.get(beacon_id, {})
        old_state = old.get("state")
        new_state = data.get("state")
        self.beacon_states[beacon_id] = {**old, **data, "id": beacon_id}

        if not self.child_is_active(beacon_id):
            self.pending_beacon_transitions.pop(beacon_id, None)
            self.beacon_attendance_notified.pop(beacon_id, None)
            self.beacon_attendance_pending.pop(beacon_id, None)
            return

        if retained and new_state in ("near", "absent"):
            self.beacon_attendance_notified[beacon_id] = new_state
            self.beacon_attendance_pending.pop(beacon_id, None)

        if retained or old_state == new_state:
            return

        # Late-state race: if bus/state hasn't arrived yet, stash the
        # transition so handle_bus_state can replay it once state is known.
        allows = self.bus_allows_attendance_or_unknown()
        if allows is None:
            self.pending_beacon_transitions[beacon_id] = (old_state, new_state, data)
            return
        if not allows:
            return
        self._emit_beacon_transition(beacon_id, old_state, new_state, data)

    def _flush_pending_beacon_transitions(self):
        if not self.pending_beacon_transitions:
            return
        if not self.bus_allows_attendance():
            return
        pending = self.pending_beacon_transitions
        self.pending_beacon_transitions = {}
        for bid, (old_state, new_state, data) in pending.items():
            self._emit_beacon_transition(bid, old_state, new_state, data)

    def _beacon_transition_key(self, beacon_id, old_state, new_state, data):
        event_ts = data.get("timestamp") if isinstance(data, dict) else None
        if not event_ts:
            event_ts = now_iso()
        return f"beacon:{beacon_id}:{old_state}->{new_state}:{event_ts}"

    def _attendance_transition_is_duplicate(self, beacon_id, new_state):
        if new_state not in ("near", "absent"):
            return False
        return (self.beacon_attendance_notified.get(beacon_id) == new_state
                or self.beacon_attendance_pending.get(beacon_id) == new_state)

    def _mark_attendance_pending(self, beacon_id, new_state):
        if new_state in ("near", "absent"):
            self.beacon_attendance_pending[beacon_id] = new_state

    def _mark_attendance_delivered(self, beacon_id, new_state, _job=None):
        if new_state in ("near", "absent"):
            self.beacon_attendance_pending.pop(beacon_id, None)
            self.beacon_attendance_notified[beacon_id] = new_state

    def _clear_attendance_pending(self, beacon_id, new_state, _job=None):
        if self.beacon_attendance_pending.get(beacon_id) == new_state:
            self.beacon_attendance_pending.pop(beacon_id, None)

    def _emit_beacon_transition(self, beacon_id, old_state, new_state, data):
        if not self.child_is_active(beacon_id):
            return
        name = self.child_name(beacon_id)
        # Attendance transitions are event-scoped. The bus state timestamp can
        # remain unchanged for hours while children enter/exit, so using it here
        # suppresses later real transitions until the 24h dedupe TTL expires.
        key = self._beacon_transition_key(beacon_id, old_state, new_state, data)

        if new_state == "near":
            if self._attendance_transition_is_duplicate(beacon_id, "near"):
                self.log("beacon_transition_skipped", reason="attendance_dedupe",
                         beacon_id=beacon_id, old_state=old_state, new_state=new_state)
                METRICS.inc("discord_send_skipped_total")
                return
            self._mark_attendance_pending(beacon_id, "near")
            queued = self.send_discord(
                self.child_channel(beacon_id),
                f"{name} entered the bus.",
                key=key,
                embeds=[self.beacon_event_embed(
                    "Child entered bus", beacon_id, data, color=0x2ECC71,
                    extra_fields=[("Transition", f"{old_state or 'unknown'} -> near")],
                )],
                on_success=lambda job, bid=beacon_id: (
                    self._mark_attendance_delivered(bid, "near", job)
                ),
                on_failure=lambda job, bid=beacon_id: (
                    self._clear_attendance_pending(bid, "near", job)
                ),
            )
            if not queued:
                self._clear_attendance_pending(beacon_id, "near")
        elif (
            new_state == "absent" or (new_state == "far" and "rssi" not in data)
        ) and (
            self.beacon_attendance_notified.get(beacon_id) == "near"
            or self.beacon_attendance_pending.get(beacon_id) == "near"
        ):
            if self._attendance_transition_is_duplicate(beacon_id, "absent"):
                self.log("beacon_transition_skipped", reason="attendance_dedupe",
                         beacon_id=beacon_id, old_state=old_state, new_state=new_state)
                METRICS.inc("discord_send_skipped_total")
                return
            self._mark_attendance_pending(beacon_id, "absent")
            record = self.roster.get(beacon_id, {})
            home_zone = record.get("home_zone_entity")
            if self.in_zone(self.school_zone):
                queued = self.send_discord(
                    self.child_channel(beacon_id),
                    f"{name} exited at school.",
                    key=key,
                    embeds=[self.beacon_event_embed(
                        "Child exited at school", beacon_id, data, color=0x2ECC71,
                        extra_fields=[("Transition", f"{old_state or 'unknown'} -> {new_state}")],
                    )],
                    on_success=lambda job, bid=beacon_id: (
                        self._mark_attendance_delivered(bid, "absent", job)
                    ),
                    on_failure=lambda job, bid=beacon_id: (
                        self._clear_attendance_pending(bid, "absent", job)
                    ),
                )
            elif self.in_zone(home_zone):
                queued = self.send_discord(
                    self.child_channel(beacon_id),
                    f"{name} reached home.",
                    key=key,
                    embeds=[self.beacon_event_embed(
                        "Child reached home", beacon_id, data, color=0x2ECC71,
                        extra_fields=[("Transition", f"{old_state or 'unknown'} -> {new_state}")],
                    )],
                    on_success=lambda job, bid=beacon_id: (
                        self._mark_attendance_delivered(bid, "absent", job)
                    ),
                    on_failure=lambda job, bid=beacon_id: (
                        self._clear_attendance_pending(bid, "absent", job)
                    ),
                )
            else:
                queued = self.send_discord(
                    self.child_channel(beacon_id),
                    f"{name} exited the bus.",
                    key=key,
                    embeds=[self.beacon_event_embed(
                        "Child exited bus", beacon_id, data, color=0x3498DB,
                        extra_fields=[("Transition", f"{old_state or 'unknown'} -> {new_state}")],
                    )],
                    on_success=lambda job, bid=beacon_id: (
                        self._mark_attendance_delivered(bid, "absent", job)
                    ),
                    on_failure=lambda job, bid=beacon_id: (
                        self._clear_attendance_pending(bid, "absent", job)
                    ),
                )
            if not queued:
                self._clear_attendance_pending(beacon_id, "absent")

    def handle_bus_event(self, data, retained=False):
        if retained:
            return
        event = data.get("event")
        if event == "bus_clear":
            # Dedupe per ignition-off stop. Use bus/state.timestamp as the
            # stable cycle marker (firmware bumps it on every state transition).
            ts = (self.bus_state.get("timestamp")
                  if isinstance(self.bus_state, dict) else None)
            key = f"bus_clear:{ts or 'unknown'}"
            self.send_discord(
                self.global_channel,
                "Bus clear: no active child beacons near and mmWave clear.",
                key=key,
                embeds=[self.bus_event_embed(
                    "Bus clear",
                    "No active child beacons are near and mmWave reports clear.",
                    color=0x2ECC71,
                )],
            )
        elif event == "beacon_silent_while_armed":
            # Advisory: a child beacon that was near at arming has gone absent.
            # Ambiguous - the child either left (good) or the beacon battery
            # died (bad - child could still be inside but invisible to BLE).
            # Surface as a non-everyone warning so the operator can confirm.
            beacon_id = data.get("beacon_id", "unknown")
            name = self.child_name(beacon_id) if beacon_id != "unknown" else "a child"
            ts = (self.bus_state.get("timestamp")
                  if isinstance(self.bus_state, dict) else None)
            key = f"beacon_silent:{beacon_id}:{ts or now_iso()}"
            self.send_discord(
                self.global_channel,
                f"Advisory: {name}'s beacon ({beacon_id}) went silent while the bus is armed. "
                f"Either the child exited, or the beacon battery died. CO2 and mmWave are still active.",
                key=key,
                embeds=[self.beacon_event_embed(
                    "Beacon silent while armed",
                    beacon_id,
                    self.beacon_states.get(beacon_id, {}),
                    color=0xF1C40F,
                    extra_fields=[
                        ("Meaning", "Child may have exited, or the beacon battery/signal failed.", False),
                        ("Other sensors", "CO2 and mmWave remain active.", False),
                    ],
                )],
            )
        elif event == "sensor_fault":
            ts = (self.bus_state.get("timestamp")
                  if isinstance(self.bus_state, dict) else None)
            key = f"sensor_fault:{ts or now_iso()}"
            self.send_discord(
                self.global_channel,
                "Warning: a critical bus sensor (LD2412 or SCD4x) is reporting a persistent fault. "
                "Safety detection is degraded - one of the three child-in-van signals may be unavailable.",
                key=key,
                embeds=[self.bus_event_embed(
                    "Sensor fault",
                    "A critical sensor reports a persistent fault. Detection is degraded.",
                    color=0xF1C40F,
                    extra_fields=[
                        ("Affected path", data.get("sensor") or data.get("detail") or "LD2412 or SCD4x", False),
                    ],
                )],
            )
        elif event == "armed":
            self.log("bus_armed")
        elif event in ("ignition_on", "ignition_off", "distress_ack",
                       "distress_clear", "disarmed"):
            self.log("bus_event", bus_event=event)

    def handle_location_event(self, data, retained=False):
        if retained:
            return
        event = data.get("event")
        zone = data.get("zone_entity")
        zone_name = data.get("zone_name") or zone
        # Dedupe based on a per-cycle marker. For zone_enter we use a single
        # "entered zone X" key per current cycle (zone tracked in zones_present).
        # For zone_leave we use the zone+the bus_state.timestamp cycle.
        cycle = (self.bus_state.get("timestamp")
                 if isinstance(self.bus_state, dict) else None) or now_iso()

        if event == "zone_enter":
            # If already in the zone (re-delivered event), don't re-notify.
            if zone in self.zones_present:
                return
            self.zones_present.add(zone)
            key = f"zone:enter:{zone}:{cycle}"
            if zone == self.school_zone:
                self.send_discord(
                    self.global_channel,
                    "Bus reached school.",
                    key=key,
                    embeds=[self.bus_event_embed(
                        "Bus reached school",
                        color=0x3498DB,
                        extra_fields=[("Zone", zone_name or zone)],
                    )],
                )
        elif event == "zone_leave":
            if zone in self.zones_present:
                self.zones_present.discard(zone)
            key = f"zone:leave:{zone}:{cycle}"
            if zone == self.school_zone:
                near_children = [self.child_name(bid) for bid, st in self.beacon_states.items()
                                 if st.get("state") == "near" and self.child_is_active(bid)]
                if near_children:
                    names = ", ".join(near_children)
                    self.send_discord(
                        self.global_channel,
                        f"Warning: bus left school while these children still appear near: {names}.",
                        key=key,
                        embeds=[self.bus_event_embed(
                            "Bus left school with children still near",
                            color=0xF1C40F,
                            extra_fields=[
                                ("Children still near", names, False),
                                ("Zone", zone_name or zone),
                            ],
                        )],
                    )
            else:
                for beacon_id, record in self.roster.items():
                    if (record.get("active", True)
                            and record.get("home_zone_entity") == zone
                            and self.beacon_states.get(beacon_id, {}).get("state") == "near"):
                        name = self.child_name(beacon_id)
                        msg = f"Warning: {name} may have missed home drop-off at {zone_name}."
                        embed = self.beacon_event_embed(
                            "Possible missed home drop-off",
                            beacon_id,
                            self.beacon_states.get(beacon_id, {}),
                            color=0xF1C40F,
                            extra_fields=[("Zone left", zone_name or zone)],
                        )
                        self.send_discord(self.global_channel, msg,
                                          key=f"{key}:global:{beacon_id}",
                                          embeds=[embed])
                        self.send_discord(self.child_channel(beacon_id), msg,
                                          key=f"{key}:child:{beacon_id}",
                                          embeds=[embed])

    def handle_ha_location_state(self, data):
        self.ha_location = {
            "source": data.get("source") or "home_assistant",
            "tracker_entity": data.get("tracker_entity"),
            "tracker_state": data.get("tracker_state"),
            "latitude": data.get("latitude"),
            "longitude": data.get("longitude"),
            "gps_accuracy": data.get("gps_accuracy"),
            "timestamp": data.get("timestamp"),
        }
        self._update_zones_from_tracker_state(data.get("tracker_state"))

    @staticmethod
    def _zone_label(value):
        return str(value or "").strip().casefold()

    def _update_zones_from_tracker_state(self, tracker_state):
        label = self._zone_label(tracker_state)
        zones = set()
        if not label or label in {"not_home", "unknown", "unavailable"}:
            self.zones_present_from_state = zones
            return

        def add_if_match(zone_entity, *names):
            labels = {self._zone_label(zone_entity)}
            if zone_entity and str(zone_entity).startswith("zone."):
                labels.add(self._zone_label(str(zone_entity).split(".", 1)[1]))
            labels.update(self._zone_label(name) for name in names if name)
            if label in labels:
                zones.add(zone_entity)

        add_if_match(self.school_zone, "school")
        for beacon_id, record in self.roster.items():
            add_if_match(
                record.get("home_zone_entity"),
                record.get("person_name"),
                record.get("id"),
                beacon_id,
            )
        self.zones_present_from_state = {zone for zone in zones if zone}

    def ha_location_text(self):
        lat = self.ha_location.get("latitude")
        lon = self.ha_location.get("longitude")
        if lat is None or lon is None:
            return None
        tracker_state = self.ha_location.get("tracker_state")
        accuracy = self.format_number(self.ha_location.get("gps_accuracy"), "m")
        parts = [f"{lat}, {lon}"]
        if tracker_state:
            parts.append(str(tracker_state))
        if accuracy:
            parts.append(f"accuracy {accuracy}")
        return " · ".join(parts)

    def handle_distress(self, data, retained=False):
        # Empty payload = distress cleared (firmware publishes "" with retain).
        if not data:
            if self.active_distress_id:
                self.log("distress_cleared", distress_id=self.active_distress_id)
            self.active_distress_id = None
            self.distress_last_sent_at = 0.0
            self.distress_last_enqueued_at.clear()
            self.distress_received_logged = False
            self.distress_acked = False
            self._save_persistent_state()
            return
        if not isinstance(data, dict):
            return
        if retained:
            self.log("retained_distress_seen", distress_id=data.get("id"))
            return
        if data.get("severity") != "emergency" or data.get("state") not in (
                "active", "distress_active", "acked"):
            return

        distress_id = data.get("id") or self.payload_hash(data)
        now = time.time()

        # Ack gate: stop re-sending @everyone once the firmware reports ack.
        if self.distress_acked or data.get("state") == "acked":
            self.distress_acked = True
            return
        # Also respect the bus/state.acknowledged flag.
        if (isinstance(self.bus_state, dict) and self.bus_state.get("distress_acknowledged")):
            self.distress_acked = True
            return

        if not self.distress_received_logged or self.active_distress_id != distress_id:
            self.log("distress_received", distress_id=distress_id,
                     trigger=data.get("trigger"))
            self.distress_received_logged = True
            METRICS.inc("distress_received_total")

        is_new = distress_id != self.active_distress_id
        if is_new:
            self.active_distress_id = distress_id
            self.distress_last_enqueued_at = {distress_id: self.distress_last_enqueued_at.get(distress_id, 0.0)}
            self._save_persistent_state()
        else:
            last_enqueued = self.distress_last_enqueued_at.get(distress_id, 0.0)
            last_attempt = max(self.distress_last_sent_at, last_enqueued)
            if now - last_attempt < self.distress_repeat_s:
                METRICS.inc("distress_deduped_total")
                self.log("distress_deduped_repeat", distress_id=distress_id,
                         remaining_s=int(self.distress_repeat_s - (now - last_attempt)))
                return

        evidence = data.get("evidence") or {}
        trigger = data.get("trigger", "unknown")
        trigger_label = self.humanize_token(trigger)
        child_context = self.distress_child_context(data)
        event_time = data.get("timestamp", now_iso())

        co2_parts = []
        co2 = self.format_number(evidence.get("co2"), "ppm")
        if co2:
            co2_parts.append(co2)
        co2_baseline = self.format_number(evidence.get("co2_baseline"), "ppm baseline")
        if co2_baseline:
            co2_parts.append(co2_baseline)
        co2_delta = self.format_number(evidence.get("co2_delta_since_armed"), "ppm since armed")
        if co2_delta:
            co2_parts.append(co2_delta)
        co2_rise_count = evidence.get("co2_consecutive_rise_count")
        if co2_rise_count is not None:
            co2_parts.append(f"{co2_rise_count} consecutive rise samples")
        co2_text = "; ".join(co2_parts)

        mmwave_parts = []
        target = self.format_bool(evidence.get("ld2412_target"))
        if target:
            mmwave_parts.append(f"target {target}")
        moving = self.format_bool(evidence.get("ld2412_moving"))
        if moving:
            mmwave_parts.append(f"moving {moving}")
        still = self.format_bool(evidence.get("ld2412_still"))
        if still:
            mmwave_parts.append(f"still {still}")
        target_duration = self.format_number(
            evidence.get("ld2412_target_duration_s"), "s", precision=1,
        )
        if target_duration:
            mmwave_parts.append(f"target duration {target_duration}")
        frame_age = self.format_number(
            evidence.get("ld2412_frame_age_s"), "s", precision=1,
        )
        if frame_age:
            mmwave_parts.append(f"frame age {frame_age}")
        moving_distance = self.format_number(evidence.get("moving_distance"), "cm")
        still_distance = self.format_number(evidence.get("still_distance"), "cm")
        if moving_distance or still_distance:
            mmwave_parts.append(
                f"distance moving {moving_distance or 'n/a'}, still {still_distance or 'n/a'}"
            )
        moving_energy = self.format_number(evidence.get("moving_energy"))
        still_energy = self.format_number(evidence.get("still_energy"))
        if moving_energy or still_energy:
            mmwave_parts.append(
                f"energy moving {moving_energy or 'n/a'}, still {still_energy or 'n/a'}"
            )
        mmwave_text = "; ".join(mmwave_parts)

        beacon_parts = []
        if child_context["beacon_ids"]:
            beacon_parts.append(", ".join(f"`{bid}`" for bid in child_context["beacon_ids"]))
        primary_beacon_id = (
            child_context["beacon_ids"][0] if len(child_context["beacon_ids"]) == 1 else None
        )
        beacon_state = evidence.get("beacon_state")
        if not beacon_state and primary_beacon_id:
            beacon_state = self.beacon_states.get(primary_beacon_id, {}).get("state")
        if beacon_state:
            beacon_parts.append(f"state {beacon_state}")
        rssi_value = evidence.get("rssi")
        if rssi_value is None and primary_beacon_id:
            rssi_value = self.beacon_states.get(primary_beacon_id, {}).get("rssi")
        rssi = self.format_number(rssi_value, "dBm")
        if rssi:
            beacon_parts.append(f"RSSI {rssi}")
        beacon_parts.append(f"source: {child_context['source']}")
        beacon_text = "; ".join(beacon_parts)

        ha_location = self.ha_location_text()

        content = (
            "@everyone EMERGENCY: possible child left in bus - "
            f"{child_context['summary']}. Trigger: {trigger_label}."
        )
        fields = [
            ("Child / beacon", child_context["summary"], False),
            ("Trigger", trigger_label),
            ("Distress ID", f"`{distress_id}`"),
            ("CO2", co2_text),
            ("mmWave", mmwave_text, False),
            ("Beacon evidence", beacon_text, False),
            ("Bus state", self.bus_state.get("state") if isinstance(self.bus_state, dict) else None),
            ("Event time", event_time, False),
            (
                "Operator action",
                "Check the bus cabin now. Acknowledge/clear in Home Assistant; "
                "use `/disarm` only after physical cabin-clear confirmation.",
                False,
            ),
        ]
        if ha_location:
            fields.append(("HA location", ha_location, False))
        embed = self.make_embed(
            "Emergency: possible child left in bus",
            "Immediate operator response required. Details are shown once here to avoid duplicating the alert text.",
            color=0xE74C3C,
            fields=fields,
        )
        def mark_distress_delivered(_job):
            if self.active_distress_id == distress_id:
                self.distress_last_sent_at = time.time()
                self._save_persistent_state()
            METRICS.inc("distress_sent_total")
            self.log("distress_sent", distress_id=distress_id, trigger=trigger)

        queued = self.send_discord(
            self.emergency_channel,
            content,
            emergency=True,
            key=None,
            embeds=[embed],
            on_success=mark_distress_delivered,
        )
        if queued:
            self.distress_last_enqueued_at[distress_id] = now
            self.log("distress_queued", distress_id=distress_id, trigger=trigger)
        else:
            self.log("distress_send_not_enqueued", distress_id=distress_id, trigger=trigger)

    @staticmethod
    def payload_hash(data):
        encoded = json.dumps(data, sort_keys=True, separators=(",", ":")).encode("utf-8")
        return hashlib.sha1(encoded).hexdigest()[:16]

    # ----- lifecycle -----
    def run(self):
        if not self.discord_token:
            self.log("startup_warning", message="DISCORD_BOT_TOKEN is not set")
        if not self.global_channel:
            self.log("startup_warning", message="DISCORD_GLOBAL_CHANNEL_ID is not set")
        if not self.emergency_channel:
            self.log("startup_warning", message="DISCORD_EMERGENCY_CHANNEL_ID is not set")
        self.log("startup", version=VERSION, base_topic=self.base_topic,
                 client_id=self.client_id, distress_repeat_s=self.distress_repeat_s)
        # Start the Prometheus HTTP server. /health is used by Docker HEALTHCHECK.
        metrics_port = env_int("METRICS_PORT", 9101)
        try:
            start_metrics_server(metrics_port)
            self.log("metrics_started", port=metrics_port)
        except OSError as exc:
            self.log("metrics_start_failed", error=str(exc))
        # LWT includes the connected flags so HA dashboards reflect real state.
        lwt_payload = {
            "status": "offline",
            "mqtt_connected": False,
            "discord_connected": False,
            "timestamp": now_iso(),
            "version": VERSION,
        }
        self.client.will_set(
            self.topic("relay/discord/status"),
            json.dumps(lwt_payload, separators=(",", ":")),
            qos=1, retain=True,
        )
        self.sender.start()
        commands_configured = (
            CHANNEL_ID_RE.fullmatch(self.command_guild_id or "") and
            bool(self.operator_user_ids or self.operator_role_ids)
        )
        if commands_configured:
            self.command_client = DiscordOperatorClient(self)
            self.command_thread = threading.Thread(
                target=self.command_client.run_blocking,
                name="discord-operator-commands",
                daemon=True,
            )
            self.command_thread.start()
        elif self.command_guild_id or self.operator_user_ids or self.operator_role_ids:
            self.log("discord_commands_disabled", reason="incomplete_or_invalid_configuration")
        try:
            self.client.connect(self.mqtt_host, self.mqtt_port, keepalive=60)
        except Exception as exc:
            self.log("mqtt_connect_failed", error=str(exc))
            # paho's loop_forever retries via reconnect on_failure internally.
        self.client.loop_forever()

    def stop(self, *_args):
        self.log("shutdown")
        try:
            self.publish_status("offline")
            self._save_persistent_state()
            self.sender.stop()
            self.sender.join(timeout=2)
            if self.command_client:
                self.command_client.stop_threadsafe()
            if self.command_thread:
                self.command_thread.join(timeout=3)
            self.client.disconnect()
        except Exception as exc:
            self.log("shutdown_error", error=str(exc))
        sys.exit(0)


def main():
    relay = DiscordRelay()
    signal.signal(signal.SIGTERM, relay.stop)
    signal.signal(signal.SIGINT, relay.stop)
    relay.run()


if __name__ == "__main__":
    main()
