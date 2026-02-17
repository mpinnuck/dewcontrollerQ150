"""
Q150DewManager.py
=================

Management app for the Q150 Dew Controller (Seeed XIAO ESP32-C3)
- Auto-connect to BLE device on startup
- Reads INFO + CONFIG
- Subscribes to STATUS notifications
- Allows editing and writing:
    - Heater enabled
    - Spread→Power table (up to 5 rows)
    - Wi-Fi SSID + Password (writes only; password never read back if firmware masks it)

Requires:
    pip install bleak


Packaging (like your FlatPanel):
    cd app
    source .venv/bin/activate
    pyinstaller q150dewcontroller.spec --noconfirm
Windows
    cd app
    .\.venv\Scripts\Activate.ps1 
    pyinstaller q150dewcontrollerw.spec --noconfirm
"""

import asyncio
import json
import tkinter as tk
from tkinter import ttk, messagebox
from bleak import BleakClient, BleakScanner, BleakError
import multiprocessing
import sys

# ======================================================
# Version
# ======================================================
VERSION = "2.0.0"


# ======================================================
# BLE UUIDs (match firmware)
# ======================================================
DEVICE_NAME = "Q150DewController"
DEVICE_MAC = "3982D22B-9B6B-D318-04BE-C3A68E539A8C"  # Known MAC address for fallback

SERVICE_UUID = "ab120000-0000-0000-0000-000000000001"
STATUS_UUID  = "ab120000-0000-0000-0000-000000000002"
CONFIG_UUID  = "ab120000-0000-0000-0000-00000000C003"
INFO_UUID    = "ab120000-0000-0000-0000-000000000004"
CMD_UUID     = "ab120000-0000-0000-0000-000000000005"


# ======================================================
# App state
# ======================================================
client: BleakClient | None = None
connected: bool = False
stop_event: asyncio.Event | None = None
reconnect_enabled: bool = True
reconnect_task: asyncio.Task | None = None
last_device = None  # Store last connected device for reconnection

latest_info: dict = {}
latest_status: dict = {}
latest_config: dict = {}


# ======================================================
# GUI globals
# ======================================================
root = None
log_text = None
conn_label = None
info_text = None

heater_enabled_var = None
heater_button = None

status_vars = {}
manual_power_var = None

table_rows = []  # list of (spread_var, power_var)
wifi_ssid_var = None
wifi_password_var = None
wifi_timezone_var = None


# ======================================================
# Logging
# ======================================================
def log(msg: str) -> None:
    if log_text is None:
        return
    try:
        log_text.configure(state=tk.NORMAL)
        log_text.insert(tk.END, msg + "\n")
        log_text.see(tk.END)
        log_text.configure(state=tk.DISABLED)
    except tk.TclError:
        # Widget has been destroyed (app is closing)
        pass


def clear_log() -> None:
    """Clear the log text widget."""
    if log_text is None:
        return
    try:
        log_text.configure(state=tk.NORMAL)
        log_text.delete("1.0", tk.END)
        log_text.configure(state=tk.DISABLED)
    except tk.TclError:
        pass


def set_connected_ui(is_up: bool) -> None:
    if conn_label is None:
        return
    if is_up:
        conn_label.config(text="✅ Connected", foreground="green")
    else:
        conn_label.config(text="❌ Disconnected", foreground="red")


# ======================================================
# JSON helpers
# ======================================================
def _safe_json_loads(b: bytes, verbose=False) -> dict:
    try:
        text = b.decode("utf-8", errors="replace")
        return json.loads(text)
    except json.JSONDecodeError as e:
        if verbose:
            text = b.decode("utf-8", errors="replace")
            log(f"❌ JSON parse error at pos {e.pos}: {e.msg}")
            log(f"   Received {len(text)} chars: '{text}'")
        return {}
    except Exception as e:
        if verbose:
            log(f"❌ Decode error: {e}")
        return {}

def _clamp_int(x: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, x))

def _try_float(s: str) -> float | None:
    try:
        return float(s)
    except Exception:
        return None

def _try_int(s: str) -> int | None:
    try:
        return int(s)
    except Exception:
        return None


# ======================================================
# BLE: status notification handler
# ======================================================
def on_status_notify(_: int, data: bytes) -> None:
    global latest_status
    st = _safe_json_loads(data)
    if not st:
        log("⚠️ STATUS decode failed")
        return

    latest_status = st

    # Update UI fields safely
    for k in ["T", "RH", "Td", "spread", "power", "enabled", "source"]:
        if k in status_vars:
            v = st.get(k, "--")
            # make it friendly formatting
            if isinstance(v, float):
                status_vars[k].set(f"{v:.2f}")
            else:
                status_vars[k].set(str(v))
    
    # Update heater button appearance based on power level
    if heater_enabled_var and heater_button:
        set_heater_button(heater_enabled_var.get())


# ======================================================
# BLE operations
# ======================================================
async def find_device_by_name(timeout: float = 6.0):
    log(f"🔍 Scanning for BLE device '{DEVICE_NAME}' ({timeout:.0f}s)...")
    
    # Try with service UUID filter first
    # devices = await BleakScanner.discover(timeout=timeout, service_uuids=[SERVICE_UUID])
    
    # If no devices found with service filter, try without filter
    #if not devices:
    #    log(f"📡 No devices found with service filter, scanning all devices...")
    devices = await BleakScanner.discover(timeout=timeout)
    
    # Log all discovered devices
    log(f"📡 Found {len(devices)} BLE device(s):")
    for d in devices:
        device_name = d.name or "(unnamed)"
        log(f"  • {device_name} [{d.address}]")
    
    # Look for our target device by name OR MAC address
    for d in devices:
        # Match by name
        if (d.name or "").strip() == DEVICE_NAME:
            log(f"✅ Found {DEVICE_NAME} at {d.address}")
            return d
        # Match by MAC address (for cached device names)
        if d.address.upper() == DEVICE_MAC.upper():
            log(f"✅ Found device by MAC address {d.address} (name: {d.name or 'unnamed'})")
            return d
    
    log(f"❌ Device '{DEVICE_NAME}' or MAC '{DEVICE_MAC}' not found in scan results")
    return None


def disconnect_callback(_client: BleakClient):
    global connected, reconnect_task, client
    connected = False
    client = None  # Clear the client reference
    set_connected_ui(False)
    log("⚠️ BLE disconnected")
    
    # Trigger reconnection attempt
    if reconnect_enabled and stop_event and not stop_event.is_set():
        if reconnect_task is None or reconnect_task.done():
            reconnect_task = asyncio.create_task(reconnect_loop())


async def connect_to_device(device):
    """Connect to a specific BleakDevice."""
    global client, connected, last_device

    # Clean up any existing client first
    if client is not None:
        try:
            if client.is_connected:
                await client.disconnect()
        except Exception:
            pass
        client = None

    try:
        client = BleakClient(device, disconnected_callback=disconnect_callback)
        log("🔌 Connecting...")
        await client.connect(timeout=10.0)
        
        # Try to get/log MTU size for debugging
        try:
            mtu = client.mtu_size
            log(f"📏 MTU size: {mtu} bytes")
        except:
            log("📏 MTU size: (unknown)")
        
        connected = True
        last_device = device  # Store for reconnection
        set_connected_ui(True)
        log("✅ Connected")

        # INFO
        await read_info()

        # CONFIG
        await read_config()

        # STATUS notify
        await client.start_notify(STATUS_UUID, on_status_notify)
        log("📡 STATUS notifications enabled")
        return True

    except (BleakError, asyncio.TimeoutError) as e:
        connected = False
        client = None
        set_connected_ui(False)
        log(f"❌ Connect failed: {e}")
        return False


async def connect_auto():
    global client, connected, reconnect_task

    if connected and client:
        return True

    d = await find_device_by_name()
    if not d:
        # Start reconnect loop if initial connection fails
        if reconnect_enabled and not stop_event.is_set():
            if reconnect_task is None or reconnect_task.done():
                reconnect_task = asyncio.create_task(reconnect_loop())
        return False

    success = await connect_to_device(d)
    if not success:
        # Start reconnect loop if connection fails
        if reconnect_enabled and not stop_event.is_set():
            if reconnect_task is None or reconnect_task.done():
                reconnect_task = asyncio.create_task(reconnect_loop())
    return success


async def reconnect_loop():
    """Automatic reconnection loop with exponential backoff."""
    global connected, reconnect_task
    
    retry_delays = [2, 5, 10, 15, 30]  # Seconds between retries
    attempt = 0
    
    while not connected and reconnect_enabled and not stop_event.is_set():
        delay = retry_delays[min(attempt, len(retry_delays) - 1)]
        log(f"🔄 Reconnecting in {delay}s... (attempt {attempt + 1})")
        
        await asyncio.sleep(delay)
        
        if stop_event.is_set() or not reconnect_enabled:
            break
        
        # Try to reconnect to last device first
        if last_device:
            log(f"🔄 Attempting reconnection to last device...")
            if await connect_to_device(last_device):
                log("✅ Reconnected successfully!")
                break
        else:
            # Try auto-discovery
            log(f"🔄 Scanning for device...")
            if await connect_auto():
                log("✅ Reconnected successfully!")
                break
        
        attempt += 1
    
    reconnect_task = None


async def connect_manual():
    """Show a dialog to select from all discovered devices."""
    log(f"🔍 Scanning for all BLE devices (6s)...")
    devices = await BleakScanner.discover(timeout=6.0)
    
    if not devices:
        messagebox.showinfo("Manual Connect", "No BLE devices found.")
        return
    
    # Create selection dialog
    dialog = tk.Toplevel(root)
    dialog.title("Select BLE Device")
    dialog.geometry("500x400")
    dialog.transient(root)
    dialog.grab_set()
    
    ttk.Label(dialog, text=f"Found {len(devices)} device(s). Select one to connect:").pack(padx=10, pady=10)
    
    # Listbox with scrollbar
    list_frame = ttk.Frame(dialog)
    list_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)
    
    scrollbar = ttk.Scrollbar(list_frame)
    scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
    
    listbox = tk.Listbox(list_frame, yscrollcommand=scrollbar.set, height=15)
    listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    scrollbar.config(command=listbox.yview)
    
    # Populate list
    device_list = []
    for d in devices:
        device_name = d.name or "(unnamed)"
        display = f"{device_name} - {d.address}"
        listbox.insert(tk.END, display)
        device_list.append(d)
    
    selected_device = [None]
    
    def on_select():
        selection = listbox.curselection()
        if not selection:
            messagebox.showwarning("Manual Connect", "Please select a device.")
            return
        selected_device[0] = device_list[selection[0]]
        dialog.destroy()
    
    def on_cancel():
        dialog.destroy()
    
    btn_frame = ttk.Frame(dialog)
    btn_frame.pack(pady=10)
    ttk.Button(btn_frame, text="Connect", command=on_select).pack(side=tk.LEFT, padx=5)
    ttk.Button(btn_frame, text="Cancel", command=on_cancel).pack(side=tk.LEFT, padx=5)
    
    # Wait for dialog to close
    await asyncio.sleep(0.1)
    while dialog.winfo_exists():
        root.update()
        await asyncio.sleep(0.05)
    
    if selected_device[0]:
        log(f"📱 Attempting to connect to {selected_device[0].name or '(unnamed)'} [{selected_device[0].address}]")
        await connect_to_device(selected_device[0])


async def disconnect():
    global client, connected, reconnect_enabled, reconnect_task
    
    # Temporarily disable auto-reconnect when user manually disconnects
    was_reconnect_enabled = reconnect_enabled
    reconnect_enabled = False
    
    # Cancel any pending reconnection task
    if reconnect_task and not reconnect_task.done():
        reconnect_task.cancel()
        reconnect_task = None
    
    if client and connected:
        try:
            log("🔌 Disconnecting...")
            await client.disconnect()
        except Exception as e:
            log(f"⚠️ Disconnect error: {e}")
    
    # Clean up client object
    client = None
    connected = False
    set_connected_ui(False)
    
    # Re-enable auto-reconnect after a short delay (if it was enabled)
    if was_reconnect_enabled:
        await asyncio.sleep(1)
        reconnect_enabled = True
        log("🔄 Auto-reconnect re-enabled")
    if was_reconnect_enabled:
        await asyncio.sleep(1)
        reconnect_enabled = True
        log("🔄 Auto-reconnect re-enabled")


async def read_info():
    global latest_info
    if not (client and connected):
        log("⚠️ Not connected")
        return
    try:
        b = await client.read_gatt_char(INFO_UUID)
        latest_info = _safe_json_loads(b)
        if latest_info:
            # Pretty text
            info_text.configure(state=tk.NORMAL)
            info_text.delete("1.0", tk.END)
            info_text.insert(tk.END, json.dumps(latest_info, indent=2))
            info_text.configure(state=tk.DISABLED)
            log("ℹ️ INFO read")
        else:
            log("⚠️ INFO decode failed")
    except Exception as e:
        log(f"❌ INFO read failed: {e}")


async def read_config():
    global latest_config
    if not (client and connected):
        log("⚠️ Not connected")
        return
    try:
        b = await client.read_gatt_char(CONFIG_UUID)
        log(f"📥 CONFIG raw bytes: {b[:100] if len(b) > 100 else b}")  # Debug: show raw data
        latest_config = _safe_json_loads(b, verbose=True)
        if not latest_config:
            log("⚠️ CONFIG decode failed - empty or invalid JSON")
            return

        log(f"✅ CONFIG read: {json.dumps(latest_config, indent=2)}")

        # Heater enabled
        heater_enabled = bool(latest_config.get("heaterEnabled", True))
        heater_enabled_var.set(heater_enabled)
        
        # Update heater button appearance
        if heater_button:
            set_heater_button(heater_enabled)

        # Table
        table = latest_config.get("table", [])
        count = latest_config.get("count", len(table))
        if isinstance(count, int):
            count = _clamp_int(count, 1, 5)
        else:
            count = _clamp_int(len(table), 1, 5)

        # Fill rows
        for i in range(5):
            spread_var, power_var = table_rows[i]
            if i < len(table):
                spread = table[i].get("spread", "")
                power = table[i].get("power", "")
                spread_var.set("" if spread is None else str(spread))
                power_var.set("" if power is None else str(power))
            else:
                spread_var.set("")
                power_var.set("")

        # WiFi Configuration
        wifi_ssid = latest_config.get("wifiSSID", "")
        if wifi_ssid_var:
            wifi_ssid_var.set(wifi_ssid)
        
        # Password field - show asterisks if password exists, otherwise empty
        wifi_password = latest_config.get("wifiPassword", "")
        if wifi_password_var:
            if wifi_password and wifi_password != "":
                # Show asterisks matching the length of the password
                # Entry widget show="*" will display any characters as asterisks
                wifi_password_var.set(wifi_password)
            else:
                # No password set
                wifi_password_var.set("")
        
        # Timezone
        wifi_timezone = latest_config.get("timezone", "AEST-10AEDT,M10.1.0,M4.1.0/3")
        if wifi_timezone_var:
            wifi_timezone_var.set(wifi_timezone)

        log("⚙️ CONFIG read")
    except Exception as e:
        log(f"❌ CONFIG read failed: {e}")


def build_config_payload_from_ui() -> dict | None:
    # Validate table rows. We will send only non-empty rows.
    entries = []
    for i in range(5):
        spread_var, power_var = table_rows[i]
        spread_s = spread_var.get().strip()
        power_s = power_var.get().strip()

        if spread_s == "" and power_s == "":
            continue

        spread = _try_float(spread_s)
        power = _try_int(power_s)

        if spread is None:
            messagebox.showerror("Invalid table", f"Row {i+1}: spread must be a number.")
            return None
        if power is None:
            messagebox.showerror("Invalid table", f"Row {i+1}: power must be an integer.")
            return None
        power = _clamp_int(power, 0, 100)

        entries.append({"spread": float(spread), "power": int(power)})

    if not entries:
        # It is valid to send only heaterEnabled, but table cannot be empty in firmware constraints.
        # We'll allow empty table if user only toggles heater.
        pass

    payload = {
        "heaterEnabled": bool(heater_enabled_var.get())
    }

    if entries:
        payload["table"] = entries

    # WiFi Configuration - only include if fields are not empty
    if wifi_ssid_var and wifi_password_var and wifi_timezone_var:
        ssid = wifi_ssid_var.get().strip()
        password = wifi_password_var.get().strip()
        timezone = wifi_timezone_var.get().strip()
        if ssid:  # Only send if SSID is provided
            payload["wifiSSID"] = ssid
            payload["wifiPassword"] = password if password else ""
            payload["timezone"] = timezone if timezone else "AEST-10AEDT,M10.1.0,M4.1.0/3"

    return payload


async def write_config():
    if not (client and connected):
        log("⚠️ Not connected")
        return

    payload = build_config_payload_from_ui()
    if payload is None:
        return

    try:
        b = json.dumps(payload).encode("utf-8")
        await client.write_gatt_char(CONFIG_UUID, b, response=True)
        log("✅ CONFIG written")
        # Refresh to reflect sorted order / normalized config
        await asyncio.sleep(0.2)
        await read_config()
    except Exception as e:
        log(f"❌ CONFIG write failed: {e}")


# ======================================================
# GUI
# ======================================================
def set_heater_button(on: bool):
    """Update heater button based on enabled state and current power."""
    if heater_button is None:
        return
    
    if not on:
        # Disabled: Red
        heater_button.config(
            text="Heater OFF",
            bg="red",
            fg="white"
        )
        heater_button.frame.config(bg="red")
    else:
        # Enabled: Check if actively heating
        power = latest_status.get("power", 0)
        if isinstance(power, (int, float)) and power > 0:
            # Actively heating: Green
            heater_button.config(
                text="Heater ON",
                bg="green",
                fg="white"
            )
            heater_button.frame.config(bg="green")
        else:
            # Enabled but not heating: Amber
            heater_button.config(
                text="Heater ON",
                bg="orange",
                fg="black"
            )
            heater_button.frame.config(bg="orange")


def toggle_reconnect():
    global reconnect_enabled
    reconnect_enabled = not reconnect_enabled
    status = "enabled" if reconnect_enabled else "disabled"
    log(f"🔄 Auto-reconnect {status}")


async def toggle_heater():
    global heater_enabled_var, heater_button
    # Toggle the state
    new_state = not heater_enabled_var.get()
    heater_enabled_var.set(new_state)
    
    # Update button appearance
    set_heater_button(new_state)
    
    # Write to device
    await write_config()
    
    log(f"🔥 Heater {'ON' if new_state else 'OFF'}")


async def set_manual_power():
    """Send manual power command to device."""
    global manual_power_var
    if not (client and connected):
        log("⚠️ Not connected - cannot set manual power")
        return
    
    power = manual_power_var.get()
    try:
        # Send command via CMD characteristic (format: "power:XX")
        cmd = f"power:{power}"
        await client.write_gatt_char(CMD_UUID, cmd.encode("utf-8"), response=False)
    except Exception as e:
        log(f"❌ Failed to set manual power: {e}")


def build_gui():
    global root, log_text, conn_label, info_text
    global heater_enabled_var, status_vars
    global table_rows, heater_button, manual_power_var
    global wifi_ssid_var, wifi_password_var, wifi_timezone_var

    root = tk.Tk()
    root.title("Q150 Dew Controller Manager")
    root.protocol("WM_DELETE_WINDOW", on_close)

    # Layout weights
    root.grid_rowconfigure(0, weight=0)
    root.grid_rowconfigure(1, weight=0)
    root.grid_rowconfigure(2, weight=1)
    root.grid_columnconfigure(0, weight=1)

    # Title bar with version
    title_frame = ttk.Frame(root)
    title_frame.grid(row=0, column=0, sticky="ew", padx=10, pady=(10, 0))
    ttk.Label(title_frame, text="Q150 Dew Controller Manager", font=("", 14, "bold")).pack(side=tk.LEFT)
    ttk.Label(title_frame, text=f"v{VERSION}", font=("", 9), foreground="gray").pack(side=tk.RIGHT)

    top = ttk.Frame(root, padding=10)
    top.grid(row=1, column=0, sticky="nsew")
    top.grid_columnconfigure(0, weight=1)
    top.grid_columnconfigure(1, weight=1)
    top.grid_rowconfigure(1, weight=1)

    # Connection bar
    conn_bar = ttk.Frame(top)
    conn_bar.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 8))
    conn_label = ttk.Label(conn_bar, text="❌ Disconnected", foreground="red")
    conn_label.pack(side=tk.LEFT)
    
    # Auto-reconnect checkbox
    auto_reconnect_var = tk.BooleanVar(value=True)
    ttk.Checkbutton(conn_bar, text="Auto-reconnect", variable=auto_reconnect_var,
                    command=toggle_reconnect).pack(side=tk.LEFT, padx=(15,0))

    ttk.Button(conn_bar, text="Manual Connect",
               command=lambda: asyncio.create_task(connect_manual())).pack(side=tk.RIGHT, padx=(5,0))
    ttk.Button(conn_bar, text="Reconnect",
               command=lambda: asyncio.create_task(connect_auto())).pack(side=tk.RIGHT, padx=(5,0))
    ttk.Button(conn_bar, text="Disconnect",
               command=lambda: asyncio.create_task(disconnect())).pack(side=tk.RIGHT)

    # Left column: Status + Info
    left = ttk.Frame(top)
    left.grid(row=1, column=0, sticky="nsew", padx=(0, 8))
    left.grid_rowconfigure(1, weight=1)

    status_frame = ttk.LabelFrame(left, text="Live Status")
    status_frame.grid(row=0, column=0, sticky="ew")

    keys = [("T", "Ambient °C"),
            ("RH", "Humidity %"),
            ("Td", "Dew Point °C"),
            ("spread", "Spread °C"),
            ("power", "Power %"),
            ("enabled", "Heater Enabled"),
            ("source", "Source")]

    for r, (k, label) in enumerate(keys):
        ttk.Label(status_frame, text=label).grid(row=r, column=0, sticky="w", padx=6, pady=2)
        v = tk.StringVar(value="--")
        ttk.Label(status_frame, textvariable=v).grid(row=r, column=1, sticky="w", padx=6, pady=2)
        status_vars[k] = v

    info_frame = ttk.LabelFrame(left, text="Device Info (INFO)")
    info_frame.grid(row=1, column=0, sticky="nsew", pady=(8,0))
    info_frame.grid_columnconfigure(0, weight=1)
    info_frame.grid_columnconfigure(1, weight=1)

    info_text = tk.Text(info_frame, height=10, width=40)
    info_text.grid(row=0, column=0, columnspan=2, sticky="nsew", padx=6, pady=6)
    info_text.configure(state=tk.DISABLED)

    ttk.Button(info_frame, text="Read INFO",
               command=lambda: asyncio.create_task(read_info())).grid(row=1, column=0, columnspan=2, sticky="ew", padx=6, pady=(0,6))

    # Heater toggle button (using tk.Label styled as button for reliable colors on macOS)
    heater_frame = tk.Frame(left, bg="red", relief=tk.RAISED, borderwidth=3)
    heater_frame.grid(row=2, column=0, sticky="ew", pady=(8,0))
    heater_button = tk.Label(
        heater_frame,
        text="Heater OFF",
        font=('Helvetica', 11, 'bold'),
        bg="red",
        fg="white",
        cursor="hand2",
        pady=6
    )
    heater_button.pack(fill="both", expand=True)
    heater_button.bind("<Button-1>", lambda e: asyncio.create_task(toggle_heater()))
    
    # Store reference to the frame for border color changes
    heater_button.frame = heater_frame

    # Right column: Config editor
    right = ttk.Frame(top)
    right.grid(row=1, column=1, sticky="nsew")
    right.grid_rowconfigure(1, weight=1)

    cfg_frame = ttk.LabelFrame(right, text="Configuration (CONFIG)")
    cfg_frame.grid(row=0, column=0, sticky="ew")
    cfg_frame.grid_columnconfigure(0, weight=1)

    heater_enabled_var = tk.BooleanVar(value=False)

    # Spread/Power table editor
    table_frame = ttk.LabelFrame(cfg_frame, text="Spread → Power Table (max 5)")
    table_frame.grid(row=0, column=0, sticky="ew", padx=6, pady=6)
    ttk.Label(table_frame, text="Spread (°C)").grid(row=0, column=0, padx=6, pady=4, sticky="w")
    ttk.Label(table_frame, text="Power (%)").grid(row=0, column=1, padx=6, pady=4, sticky="w")

    table_rows = []
    for i in range(5):
        sp = tk.StringVar()
        pw = tk.StringVar()
        table_rows.append((sp, pw))
        e1 = ttk.Entry(table_frame, textvariable=sp, width=12)
        e2 = ttk.Entry(table_frame, textvariable=pw, width=12)
        e1.grid(row=i+1, column=0, padx=6, pady=2, sticky="w")
        e2.grid(row=i+1, column=1, padx=6, pady=2, sticky="w")

    # WiFi Configuration section
    wifi_frame = ttk.LabelFrame(cfg_frame, text="WiFi Configuration")
    wifi_frame.grid(row=1, column=0, sticky="ew", padx=6, pady=(6,6))
    wifi_frame.grid_columnconfigure(1, weight=1)
    
    ttk.Label(wifi_frame, text="SSID:").grid(row=0, column=0, padx=6, pady=4, sticky="w")
    wifi_ssid_var = tk.StringVar()
    ttk.Entry(wifi_frame, textvariable=wifi_ssid_var, width=30).grid(row=0, column=1, padx=6, pady=4, sticky="ew")
    
    ttk.Label(wifi_frame, text="Password:").grid(row=1, column=0, padx=6, pady=4, sticky="w")
    wifi_password_var = tk.StringVar()
    ttk.Entry(wifi_frame, textvariable=wifi_password_var, width=30, show="*").grid(row=1, column=1, padx=6, pady=4, sticky="ew")
    
    ttk.Label(wifi_frame, text="Timezone:").grid(row=2, column=0, padx=6, pady=4, sticky="w")
    wifi_timezone_var = tk.StringVar()
    timezone_combo = ttk.Combobox(wifi_frame, textvariable=wifi_timezone_var, width=28, state="readonly")
    timezone_combo['values'] = (
        "UTC0",
        "GMT0BST,M3.5.0/1,M10.5.0",
        "CET-1CEST,M3.5.0,M10.5.0/3",
        "EET-2EEST,M3.5.0/3,M10.5.0/4",
        "EST5EDT,M3.2.0,M11.1.0",
        "CST6CDT,M3.2.0,M11.1.0",
        "MST7MDT,M3.2.0,M11.1.0",
        "PST8PDT,M3.2.0,M11.1.0",
        "AKST9AKDT,M3.2.0,M11.1.0",
        "HST10",
        "AEST-10AEDT,M10.1.0,M4.1.0/3",
        "ACST-9:30ACDT,M10.1.0,M4.1.0/3",
        "AWST-8",
        "NZST-12NZDT,M9.5.0,M4.1.0/3",
        "JST-9",
        "CST-8",
        "IST-5:30",
        "<+03>-3"
    )
    timezone_combo.current(10)  # Default to Australia (Sydney/Melbourne)
    timezone_combo.grid(row=2, column=1, padx=6, pady=4, sticky="ew")

    btns = ttk.Frame(cfg_frame)
    btns.grid(row=2, column=0, sticky="ew", padx=6, pady=(0,6))
    btns.grid_columnconfigure(0, weight=1)
    btns.grid_columnconfigure(1, weight=1)

    ttk.Button(btns, text="Read CONFIG",
               command=lambda: asyncio.create_task(read_config())).grid(row=0, column=0, sticky="ew", padx=(0,4))
    ttk.Button(btns, text="Write CONFIG",
               command=lambda: asyncio.create_task(write_config())).grid(row=0, column=1, sticky="ew", padx=(4,0))

    # Manual heater power slider (0-100%) - in right frame to align with heater button
    power_slider_frame = ttk.LabelFrame(right, text="Manual Power")
    power_slider_frame.grid(row=1, column=0, sticky="sew", pady=(8,0))
    manual_power_var = tk.IntVar(value=0)
    
    def update_power_label(v):
        power_label.config(text=f"{int(float(v))}%")
        asyncio.create_task(set_manual_power())
    
    power_slider = ttk.Scale(
        power_slider_frame,
        from_=0,
        to=100,
        orient=tk.HORIZONTAL,
        variable=manual_power_var,
        command=update_power_label
    )
    power_slider.pack(fill="x", padx=6, pady=6)
    
    power_label = ttk.Label(power_slider_frame, text="0%")
    power_label.pack(pady=(0,6))

    # Log at bottom
    log_frame = ttk.LabelFrame(root)
    log_frame.grid(row=2, column=0, sticky="nsew", padx=10, pady=(0,10))
    log_frame.grid_rowconfigure(1, weight=1)
    log_frame.grid_columnconfigure(0, weight=1)

    # Log header with label and clear button
    log_header = ttk.Frame(log_frame)
    log_header.grid(row=0, column=0, sticky="ew", padx=6, pady=(6,0))
    log_header.grid_columnconfigure(0, weight=1)
    
    ttk.Label(log_header, text="Log", font=('Helvetica', 10, 'bold')).pack(side=tk.LEFT)
    ttk.Button(log_header, text="Clear Log", command=clear_log).pack(side=tk.RIGHT)

    log_text = tk.Text(log_frame)
    log_text.grid(row=1, column=0, sticky="nsew", padx=6, pady=6)
    log_text.configure(state=tk.DISABLED)

    root.eval("tk::PlaceWindow . center")


def on_close():
    # stop asyncio loop gracefully
    if stop_event:
        stop_event.set()


# ======================================================
# Async main loop (same pattern you used)
# ======================================================
async def ui_loop():
    while not stop_event.is_set():
        try:
            root.update()
        except tk.TclError:
            break
        await asyncio.sleep(0.05)


async def main():
    global stop_event
    stop_event = asyncio.Event()
    build_gui()

    # Auto-connect at startup
    asyncio.create_task(connect_auto())

    await ui_loop()

    # Cleanup
    try:
        if client and connected:
            await client.stop_notify(STATUS_UUID)
            await client.disconnect()
    except Exception:
        pass

    try:
        root.destroy()
    except Exception:
        pass


if __name__ == "__main__":
    multiprocessing.freeze_support()
    asyncio.run(main())
    