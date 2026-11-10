# FEATURES — ESP32 WiFi Repeater (Ibn e Ilyas Technologies)

Add every new feature to this file as it's built, so it stays an
up-to-date A-to-Z record of what the repeater can do.

Current firmware version: see `FW_VERSION` in `main/main.c` (shown in the
admin dashboard header too).

---

## Core networking
- WiFi repeater (AP+STA) with real internet sharing via NAT/NAPT
  (ESP-IDF `LWIP_IPV4_NAPT`, not the Arduino core — that's what makes
  actual internet sharing possible, unlike most ArduinoDroid builds)
- Configurable DNS server handed to AP clients (default 8.8.8.8) — fixes
  the classic "connected, no internet" symptom
- Configurable hostname shown in the main router's own client list
- Configurable repeater IP/subnet, SSID, password, hidden SSID, channel,
  max connected devices, TX power
- Optional custom MAC address on the upstream (STA) side
- Optional **static IP on the main router's network** (Remote-management
  IP, Gateway, Subnet mask settings) — for predictable admin access when
  running 2+ repeaters on the same network
- Auto-restart on a schedule (0–168 hours, 0 = off) to keep memory/WiFi
  fresh on long-running installs

## Guest access & security
- MAC whitelist with a friendly name per device
- **Guest mode**: devices without a whitelisted MAC can still join with
  the WiFi password and reach the admin/tools pages, but get **no
  internet** — enforced at the WiFi-driver packet level (`ap_rx_filter`),
  not just a login wall
- **Website block list**: up to 20 domains (Advanced Configuration > Website
  Block List). The repeater drops the DNS lookups (UDP port 53) of those
  domains and their subdomains for every device. Limits: Private DNS / DoH
  bypass it, and a site whose IP is already cached on the phone may still open
  until the cache expires. The list is not part of Config Backup yet
- **Request access**: a guest can type a name on the login page; the repeater
  identifies the device by its DHCP lease (the guest never types a MAC) and the
  admin approves or rejects it under MAC Whitelist. Pending requests live in
  RAM only (8 slots)
- **First seen / connected since** per device in Connected Devices, with a
  "New" tag for guests seen in the last 10 minutes (RAM only)
- **Whitelist time window**: an optional `HHMM-HHMM` per whitelisted device
  (e.g. 2000-2300). Outside the window the device is treated as a guest (local
  access, no internet). Uses the `tz_offset` setting (hours from UTC, default 5)
  and is not applied until the clock has synced over the internet
- Optional **per-device upload speed limit** (Kbps) for whitelisted
  devices, enforced with a token-bucket limiter
- Optional **overall bandwidth cap** shared by all connected devices
  combined, set separately for upload and download (Advanced Settings >
  "Overall upload/download cap") — this one genuinely covers both
  directions, since it's enforced on the whole repeater's traffic rather
  than trying to split it per device
- Deauthentication/disassociation attack monitor (passive promiscuous
  sniffing) with a live "attack / watch / ok" status and recent-events log
- MAC-based Kick button (disconnects a currently-connected device)
- Hardware factory reset: hold the BOOT button ~8 s, release when the
  blue LED blinks fast — wipes all settings back to defaults. A reset
  counter (separate from the wiped settings) proves a reset really
  happened even when a value looks unchanged because it matched the
  factory default anyway — shown on the login page footer and in `/api/me`

## Admin dashboard (`/admin`, login required)
- Router-style web interface (Huawei HG8546M-inspired, own branding):
  blue top bar, left icon menu (Home Page, System Info, Advanced
  Configuration), topology Home page. Login, admin and `/tools` pages
  share the same look and render the same on Android and desktop
  (fixed 1000 px layout width)
- Fixed username `Sarfraz` (not editable — by design), changeable
  password, default `admin`
- Password show/hide toggle on every password field
- Live status: upstream connection, session uptime, repeater IP, remote
  management IP, free memory
- Connected-devices table: name, MAC, IP, signal (dBm), internet
  allowed/blocked
- **Device names and vendor**: each connected device shows the whitelist
  name if it has one, otherwise the hostname the phone/laptop sends in its
  DHCP request (option 12, printable ASCII only, up to 31 chars), otherwise
  just the MAC. Next to the MAC the company is shown, looked up offline
  from an IEEE OUI table built into the firmware (8925 entries, about
  44 KB of flash). Phones using a random/private MAC show "Private MAC"
  instead of a company, since the real vendor can't be known
- **Phone model and OS**: below the MAC line, Connected Devices shows
  brand, model and OS when they can be learned, e.g. `Samsung · Galaxy A32
  (SM-A325F) · Android 13`. Sources: the browser User-Agent seen in plain
  HTTP requests on port 80 (Android's connectivity check is enough, the
  phone need not open the admin page), the User-Agent of requests to the
  repeater's own pages, and DHCP option 60 (Android version / Windows).
  Friendly names come from a table generated from Google's supported-devices
  list (Samsung, Xiaomi/Redmi/POCO, OPPO, realme, vivo, Infinix, Tecno, itel,
  Motorola, OnePlus, Huawei, Honor, Google, Nokia); a model missing from it
  shows the raw code. Limits: recent Chrome hides the model in its
  User-Agent, iPhones never reveal their model over WiFi (only
  "Apple · iPhone · iOS"), and nothing shows until the device has made at
  least one HTTP request. Held in RAM only (16 devices), cleared on reboot
- **Upstream stability log**: recent disconnects with plain-English
  reason (wrong password, router off, signal lost, etc.)
- **Data usage** per whitelisted device: this week / this month /
  all-time (upload-side only — see note in Known limitations)
- **Internet speed history** chart: latency sample every ~15 min,
  last ~12 hours, red bar = internet was down at that check
- **Share this WiFi**: one-tap copy/share of the repeater's own SSID +
  password (native share sheet where supported)
- **Backup upstream network (failover)**: set a second router under Upstream WiFi.
  After 5 failed connection attempts in a row the repeater switches to the other
  network (and back again if that one fails too). The status page shows which
  one is active. Included in config export/import.
- Config backup: export/import all settings as JSON (includes the website block list)
- **Firmware update (OTA)**: upload a new `esp32-wifi-repeater.bin` from
  Advanced Configuration > Firmware Update (login required). The firmware
  checks the file size and the first byte (0xE9), validates the image,
  switches to the other app slot and restarts. Two app slots of 0x1E0000
  bytes each; rollback is enabled, so a new firmware that never gets its
  web server up is reverted to the previous one on the next boot. Use the
  plain app `.bin` from the release, not the full-flash image. Note: the
  first flash with the new partition table must be done over USB/serial
  (see README); the OTA upload itself is not yet tested on a real device
- Advanced Settings panel: every setting above, all with visible
  defaults and a "Fill defaults" button
- Reboot button, Factory Reset button (software-triggered, same effect
  as the hardware button)

## Public WiFi Tools page (`/tools`, no login needed)
- **WiFi Analyzer**: scans nearby networks (hidden SSIDs included —
  shown by BSSID even with no name), channel-congestion chart, security
  type, 802.11 mode, WPS flag, country code, router-company lookup
  (offline OUI list + automatic online lookup, with a clear banner if
  the browsing device itself has no internet to do the online part)
- **Deauth monitor** (same detector as the admin dashboard, public read)
- **Speed test**: repeater↔phone WiFi link speed, and real internet
  speed test (download/upload/latency/jitter)
- **Diagnose**: step-by-step check of upstream WiFi → gateway → internet
  → DNS, with plain-English fixes for whichever step fails
- "About Developer" popup (contact + skills) on the login page

## Under the hood / engineering notes
- Login uses session cookies, not the browser's native popup — a
  proper branded page instead
- SNTP time sync (needed for weekly/monthly usage rollover) starts
  automatically once the repeater gets internet
- LWIP TCP buffers tuned up for meaningfully faster throughput/speed
  tests than ESP-IDF's small defaults
- Single self-contained flash image (`full-flash-padded-0x0.bin`) built
  automatically by GitHub Actions on every push, sized to survive flaky
  Android flashing apps. It works on a fresh board with the current
  partition table, but it erases saved settings (WiFi, whitelist,
  password); for an already-running repeater use OTA instead

---

## Known limitations (being upfront about these)
- **Data usage tracks upload only.** The WiFi-driver hook this firmware
  uses can only see traffic *from* a client heading out; ESP-IDF has no
  safe public hook to also measure download traffic per device without
  risking the stability of the whole repeater. Totals are still useful
  as a relative "who's using this the most" indicator, just not a full
  billing-accurate figure.
- **Bandwidth limiting is upload-only for the same reason** — a
  device's uploads can be throttled, downloads/streaming cannot be,
  on this hardware/SDK combination.
- **Ethernet port: not implemented in software yet.** The plain ESP32
  chip *does* have a built-in Ethernet MAC, but needs an external PHY
  chip (e.g. LAN8720) wired to specific pins — this is a hardware
  addition, not just a firmware change. See chat for details; can be
  scoped as a dedicated follow-up once the PHY module is in hand.
- **Google Drive backup sync: Google Cloud Console setup done, device-side
  OAuth not built yet.** This needs a careful, dedicated implementation
  pass (device-code OAuth flow, token storage/refresh, Drive API calls) -
  deliberately phased separately rather than rushed into an already large
  change set. See chat for the setup steps already completed.
