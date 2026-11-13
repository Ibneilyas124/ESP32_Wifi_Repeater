# WIFI REPEATER HANDOVER v3 (FINAL) - ESP32 NAT Repeater (Sarfraz Qureshi / "IBN E ILYAS Technologies")

Tayyar: 2026-11-12 | FW_VERSION "2.1.0" | Online-update build number FW_BUILD (workflow "Stamp build number" se)
Source: v2 handover + 2026-11-11/12 ka poora chat. Jis cheez ko Claude ne khud nahi dekha uske saamne "(Sarfraz ne confirm kiya)" ya "(unverified)" likha hai.

**HEAD commit:** yeh file likhte waqt Claude ko aakhri commit ka hash pata nahi (router panel commit, `4d741bb` ke baad). Naye chat mein `git log -3 --oneline` se dekho.
**Sab se pehle yaad rakho:** Sarfraz ne aakhri build device par chala kar kaha "Repeater perfectly kaam kar raha hai". Is liye neeche ki sab cheezein device par chal rahi hain. Lekin **build-135 ke baad ke commits ka Actions green Claude ne API se nahi dekha** (device par chalna hi saboot hai). Naye chat mein pehle HEAD ka run API se check karo.

---

## 0. NAYE CHAT MEIN PEHLE YEH KARO

1. Yeh poori file paste karo.
2. Claude section 12 ka command chalwaye (fetch + status + log + Actions HEAD run + latest release). Fetch fail ho to "sync hai" / "green hai" mat kehna.
3. Phir Sarfraz se poocho ke aaj kya karna hai. Section 9 mein baqi kaam ki list hai.

---

## 1. CLAUDE KE LIYE RULES (Sarfraz ki pasand)

**Format**
- Jawab Roman Urdu / Hinglish mein.
- Termux command ek-ek block mein, code block with language tag. Explanation code block ke bahar.
- Sarfraz ke paas LAPTOP NAHI, sirf Android phone (Termux). Browser mein admin `192.168.4.1/admin`.
- Sarfraz ki Claude usage limit kam hoti hai: kam se kam messages, kam commands.
- Screenshot bhejte waqt IMEI / private cheez crop kar de (aik baar IMEI nazar aa gaya tha).

**Commands**
- Har command ka output auto-copy: `{ ... ; } 2>&1 | tee ~/work-tmp/last_out.txt`, phir `termux-clipboard-set < ~/work-tmp/last_out.txt && echo "COPIED OK" || echo "COPY FAILED"`.
- Temporary files `~/work-tmp` mein, `/tmp` NAHI.
- Checkpoint (`echo "CHECKPOINT: ..."`) usi block ke andar.
- Jo parhna ho woh AIK bare read-only command mein mango.
- Har step ke baad HANDOVER.md update + commit + push usi command ke andar.

**Git**
- Push se pehle `git pull --rebase` (network fail ho to 3 baar retry; aik baar `HTTP2 framing layer` error aaya, sirf network ka masla tha). Force push KABHI nahi.
- Remote ab `Ibneilyas124/ESP32_Wifi_Repeater` hai. Purana naam 301 redirect deta hai. API calls mein `curl -sL` (-L zaroor) aur naya repo naam.
- GitHub Actions GREEN hone tak agla phase nahi. Script khud HEAD ka run dhoondti hai (`grep -A10 $HEAD_SHA`, `"conclusion": "success"`), warna ABORT/BUILD FAILED.
- Sarfraz "Greened successfully" likhta hai aur output paste nahi karta. Jo output dekha nahi use "verified" mat kaho; Actions API se khud check karwao.
- Har push naya build `build-N+1` banata hai (doc-only bhi).

**Edit scripts (jo pattern is chat mein chala)**
- Jis hissay ka code dekha nahi us par edit na likho; pehle read-only dump mango.
- `~/work-tmp/edit.js` (node) mein anchors ki list; har anchor ka count exactly 1, warna `ABORT`, kuch nahi likhna. Replace mein `() => y` istemal karo (taake `$` special na ho).
- admin.html badalne par script blocks nikaal kar `node --check`: fail ho to `git checkout -- main/admin.html main/main.c` aur ABORT.
- ESP-IDF build `-Werror=all`: `-Werror=format-truncation` bhi. snprintf destination worst-case se bara rakho. unused-function/variable sirf warning.
- **Header ki ghalti jo hui:** `#include <poll.h>` fail hua; ESP-IDF mein `#include <sys/poll.h>` chahiye.
- Naya HTTP endpoint jodte waqt `max_uri_handlers` (ab 36) check karo. Is chat mein naya endpoint nahi joda (labels `/api/whitelist` ki `action:"label"` se, password `/api/config/import` se).
- Fix likhne se pehle wajah ko "andaza" kaho agar pakki nahi; jo dekha nahi usay "verified" mat likho.

**Sachchai**
- Apni ghalti turant maano (is chat ki: Android 10 ka source galat bataya tha, repo rename par `-L` bhoolna, poll.h).
- Andaze ko andaza kaho (misal: heap hadd 60000/24000).

---

## 2. PROJECT

- Repo: https://github.com/Ibneilyas124/ESP32_Wifi_Repeater (public); Termux folder `~/esp32-wifi-repeater`
- ESP-IDF v5.2, ESP32 DevKit V1 4MB, NAT/NAPT repeater. Firmware GitHub Actions se; har push par release `build-N`.
- Files: `main/main.c`, `main/admin.html`, `main/login.html`, `main/tools.html`, `main/oui_table.h`, `main/model_table.h`, `main/repeater_lwip_hooks.c/.h` (sirf 7 lines, koi DNS filter nahi), `main/build_num.h` (workflow overwrite karta hai), `main/CMakeLists.txt`, `partitions.csv`, `sdkconfig.defaults`, `flash.sh`, docs, `dev-tests/`.
- `main/CMakeLists.txt` REQUIRES: esp_wifi esp_netif esp_event nvs_flash esp_http_server json lwip esp_timer esp_hw_support driver app_update esp_http_client esp_https_ota esp-tls mbedtls heap.
- `sdkconfig.defaults`: `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` + `..._DEFAULT_FULL=y`, `CONFIG_APP_ROLLBACK_ENABLE=y`.
- Defaults: WiFi `Sarfraz` / `Sarfraz1`, admin login `Sarfraz` / `admin`. Repeater AP IP 192.168.4.1 (setting `ap_ip`). DNS jo clients ko milta hai: `dns1` (default 8.8.8.8). Repeater ka DHCP hostname setting `hostname`.
- Release assets: `esp32-wifi-repeater.bin` (online/manual update ke liye), `full-flash-0x0.bin`, `full-flash-padded-0x0.bin`, `bootloader.bin`, `partition-table.bin`. **Update ke liye sirf `esp32-wifi-repeater.bin`**, full-flash wali nahi.
- Admin menu SECS: home p-home; sys p-info,d-usage,d-speed,d-stab,d-diag; adv d-wifi,d-wl,d-block,d-adv,d-backup,d-ota,d-danger.

---

## 3. IS CHAT MEIN KYA BANA / FIX HUA (2026-11-11 se 11-12)

Sab Sarfraz ne device par chala kar confirm kiya, jahan likha ho "Claude ne dekha" wahan screenshot Claude ne khud dekha.

### 3.1 Online update (PASS)
- Admin > Advanced > Firmware Update: "Check for update" / "Update now". `upd_task` stack 10240; `upd_check` GET `releases/latest`, redirect ke `/tag/build-N` se N parhta hai, FW_BUILD se muqabla; `upd_install` `esp_https_ota` se.
- Jo masle aaye aur fix hue:
  1. "clock not synced": NTP (UDP 123) upstream par band. Fix: HTTP `Date` header se clock (`upd_http_time`, connectivitycheck.gstatic.com phir www.google.com).
  2. `ESP_ERR_HTTP_CONNECT`: error ab `DNS fail (host N)` ya `DNS ok, connect: ...` alag batata hai (commit `03116e3`). Saath mein update task STA par fallback DNS 8.8.8.8 set karta hai. Natija: clock sync hui.
  3. `HTTP 301`: `UPD_BASE_URL` purane repo naam par tha. Ab `https://github.com/Ibneilyas124/ESP32_Wifi_Repeater/releases/` (commit `3ced18e`). Repo dobara rename ho to yeh `#define` badalna zaroori hai.
- Device par "Check" Claude ne screenshot mein dekha ("Up to date (build #131)"). "Update now" Sarfraz ne practically chala kar confirm kiya (screenshot Claude ne nahi dekha).
- Heap (Claude ne dekha): normal ~126-141k; "Check" ke dauran minimum dekha 73392. **Install ke dauran ka heap napa nahi gaya.** Hadd free>=60000, largest block>=24000 andaza hai, tune nahi hui.
- **Rollback test (kharab image) abhi nahi hua.**

### 3.2 One-Click Diagnosis "Timed out" (FIXED)
- Wajah (andaza): lwIP blocking `connect()` `SO_SNDTIMEO` nahi manta. Fix: `tcp_probe` ab non-blocking connect + `poll()` (`#include <fcntl.h>`, `<sys/poll.h>`, `<errno.h>`). Ab 15 sec ke andar nateeja aata hai. Speed-history sampling bhi isi `tcp_probe` ko istemal karti hai.
- Diagnosis mein ESP32 khud `gethostbyname("google.com")` karta hai (STA ka DNS), fallback 8.8.8.8 wahan nahi lagta (neeche 4.1 dekho).

### 3.3 Phone model / OS detection
- Claude ne dekha: Motorola one 5G (Android 11) par DHCP hostname (`name`) khali, UA se model khali (naya Chrome "K"), reconnect par **randomized MAC** (`vendor: "Private MAC"`), isliye brand bhi khali.
- **Android version fix (device par dekha):** pehle `os` "Android 10" tha (Chrome ka frozen UA). `devinfo_get` mein ab DHCP option 60 (`android-dhcp-11`) UA se pehle chalta hai. Ab "Android 11" dikhta hai (commit `7a09e14`, build-135, Actions API se GREEN verified).
- `host_model_guess()` hostname brand-prefix (moto, galaxy, redmi, ...) se model nikalta hai; is phone par hostname aata hi nahi, to khali.
- Nateeja: kai phones ka model automatic nahi nikal sakta. Isliye **device label** feature banaya (3.4).

### 3.4 Device label (admin ka diya naam)
- Home > Connected Devices mein har row par **Name** button. NVS key `labels`, format `MAC=naam|MAC=naam|`, max 1024 bytes, naam 31 char, `| = ,` space ban jate hain.
- Whitelist se alag: access (Internet OK / Guest) par asar nahi.
- API: maujooda `/api/whitelist` POST `{action:"label", mac, name}`; khali naam = label hata do.
- Naam ki priority: whitelist naam, phir label, phir DHCP hostname.
- **Limit:** label MAC se juda hai; phone WiFi "Forget" kare to randomized MAC badal sakta hai, naam dobara dena hoga. **Labels Config Export/Import mein shamil NAHI.** `label_get` har client ke liye NVS parhta hai (admin page ke liye theek, hot path nahi).

### 3.5 Factory reset ab upstream bhi saaf karta hai
- Wajah (andaza, ab confirm): WiFi driver apni NVS copy rakhta hai. `factory_reset_post_handler` mein `esp_wifi_restore()` jora. Reset ke baad upstream khali aur "upstream offline" dikhta hai (Claude ne screenshot dekha).
- **Hardware button (BOOT ~8s) reset ka raasta check nahi hua.** `wipe_settings()` ke callers ka dump kabhi nahi dekha; ho sakta hai wahan `esp_wifi_restore()` na lagi ho. Test karo aur zaroorat ho to wahan bhi lagao.
- Reset ke baad internet band; `192.168.4.1/admin` se upstream dobara dena hota hai.

### 3.6 Backup ab file se
- **Export Config** `repeater-backup-<tareekh>.txt` download karta hai (box mein text bhi). Import ke liye "Or import from a saved backup file" file picker, confirm ke baad `importCfg()`. Server ka JSON format wahi.
- **Ehtiyaat:** backup mein upstream WiFi aur admin password plain text hota hai. File share na karo.

### 3.7 Home ke UI badlaav
- "Network connection status": "upstream offline" ab red (`#d32f2f`, inline style, `netStatus` line ~402). Sirf do halat hain (normal / upstream offline). Backup par chalna alag rang mein nahi.
- **Router ke neele jism par kahin bhi click (RESET icon par bhi)** => neeche "Repeater Management" panel (`#rtPanel`): Change Admin Password (New/Confirm, Save/Cancel), Restart, Restore Defaults.
  - Password `post('/api/config/import', {admin_pass: v})` se jata hai (4-32 char, wahi validation). Save ke baad reboot, naye password se login.
  - Restart = `doReboot()`, Restore Defaults = `doFactoryReset()` (dono ke apne confirm).
  - Panel sirf jism par dobara tap ya Cancel se band hota hai.
- **Left ke grey dot** (SVG `circle cx=256 cy=284`, click area r=14) par click => "Reboot the repeater now?" confirm, phir reboot.
- SVG: neela jism `rect x=190 y=250 w=400 h=62`; clickable groups `class="clk"` (`goTo('d-wifi')`, `toggleDevs()`, `toggleRtPanel()`, `doReboot()`).

---

## 4. ABHI KHULE MASLE / AGLE KAAM (priority se)

### 4.1 Subnet overlap (asli bug, code mein fix NAHI hua)
- Jab upstream router bhi 192.168.4.x par tha aur repeater ka AP bhi 192.168.4.1: diagnosis mein gateway "192.168.4.1, 0 ms" (apne hi AP se connect), DNS lookup fail. Upstream 192.168.100.x par aane se masla gayab (Claude ne screenshot dekha: gateway 192.168.100.1, 18 ms, DNS pass).
- Wajah overlap thi, yeh mera andaza hai (~85% pakka); Sarfraz ne upstream badla tha, `ap_ip` nahi.
- Fix ke options: (a) upstream ka subnet AP se milta ho to repeater khud AP IP badal le (`192.168.4.x` <-> `192.168.50.x`), ya (b) admin mein warning. `ap_ip` setting ka code dekha nahi (reboot/validation ka pata nahi).
- Online update isliye chalta raha ke STA par fallback DNS 8.8.8.8 hai.

### 4.2 Diagnosis DNS check fallback ke saath chalao (chhota kaam).

### 4.3 Labels ko Config Export/Import mein shamil karna.

### 4.4 Rollback test aur install ke dauran free_heap napna (Section 5, test 1).

### 4.5 SNTP sirf aik baar boot par (`s_time_synced`). Upstream par UDP band ho to time kabhi sync nahi hoti; usage week/month counters aur whitelist schedule ko time chahiye. Idea: boot par bhi HTTP Date fallback (`upd_http_time` pehle se maujood).

### 4.6 Model detection ke liye mumkin extra raaste: HTTPS par client hints (repeater HTTP par hai), Firefox / Samsung Internet UA mein asli model hota hai. Maine sirf aik phone dekha hai, dusre phones par natija mukhtalif ho sakta hai.

---

## 5. REMAINING TESTS (jo is chat mein nahi hue)

1. Rollback: kharab image ka haal; install ke dauran `/api/status` `free_heap` (browser mein `192.168.4.1/api/status` reload; admin login zaroori).
2. Hardware BOOT reset (8s) ke baad upstream khali hota hai ya nahi.
3. FLASH_AND_TEST.md ke purane tests: config export/import (blocklist), first seen / "New" tag, whitelist schedule (time sync zaroori), Website Block List (DNS UDP 53 drop), failover + primary-return, One-Click Diagnosis, tools page (/tools) phone par.
4. Audit baqi: Section 7.
5. FEATURES.md / FLASH_AND_TEST.md / README mein naye features nahi likhe: online update, request-on-Home, device label, backup file, router panel, red status. Likhna baqi.

---

## 6. FEATURES (mukammal list)

Huawei-style design (login, admin, tools), Connected Devices (DHCP hostname, UA brand/model/OS, OUI vendor, first seen, ab Android version DHCP option 60 se, device label), OTA manual upload (/api/ota, ota_0 0x20000 + ota_1 0x200000, rollback), **Online update (GitHub)**, guest "Request access" (Home par `#reqList`), Whitelist + schedule, Website Block List, Failover backup upstream + primary-return, One-Click Diagnosis, Config Export/Import (file se, blocklist shamil), Repeater Management panel (router click), factory reset (driver WiFi bhi saaf), upstream offline red status. Tafseel purani handover / FEATURES.md mein.

---

## 7. AUDIT (ESP32 par baqi dekhna)

1. `nvs_get_string` mein RAM cache nahi: `ap_rx_filter` hot path mein har packet par NVS parhta hai. Pehle speed naapo, phir cache. (Naya `label_get` bhi har client par NVS parhta hai, par woh admin status ke liye hai.)
2. WiFi task stack aur `button_task` stack 3072.
3. `WIFI_EVENT_STA_DISCONNECTED` handler event task mein sleep karta hai; upstream flap par late ho sakta hai.
4. `primary_return_task` ka `clear_ap_list` admin scan saaf kar sakta hai.
5. SNTP sirf aik baar (4.5).
6. `upd_task` heap/stack device par.

---

## 8. CHHOR DIYA / NAHI BANEGA (jab tak Sarfraz khud na kahe, dobara mat sujhao)

Per-device download count; Telegram alert; Google Drive backup; AI chat page; Ethernet port.

---

## 9. WEB UI AUR API

Admin APIs (login zaroori): /api/status /api/whitelist (add/remove/label) /api/settings /api/wifi /api/usage /api/speedhistory /api/kick /api/reboot /api/factory_reset /api/config/export /api/config/import /api/logout /api/me /api/ota /api/blocklist /api/requests /api/update /api/update/start. Bina login: /api/request, /api/speedhistory. Tools: /api/scan(+/start) /api/deauth /api/ping /api/speed /api/upspeed /api/diag(+/start). `require_session_api(req)` har login-wala handler ki pehli line; helpers `send_result`, `send_json`, `restart_soon`, `read_body`.
`/api/status` mein ab: fw, sta_connected, sta_ip, uptime_s, free_heap, sta_ssid, on_backup, sta_ssid2, ap_ip, clients[{mac,ip,name,vendor,model,first_ago_s,conn_s,os,brand,rssi,authorized}], whitelist, session_uptime_s, discon_total, discon_log. (Build number `cur` sirf `/api/update` mein hai; `/api/status` mein nahi. Chahein to status mein jod sakte hain.)
`/api/update` states: 0 idle, 1 checking, 2 available, 3 up to date, 4 installing, 5 done, 6 error.

---

## 10. LIMITS

NAPT ESP-IDF mein (Arduino core mein disabled). MAC whitelist 100% security nahi. Vendor/model/OS/labels: vendor-model-OS sirf RAM (16 entries), labels NVS mein. Hidden primary SSID par primary-return nahi. Pending guest requests RAM mein, reboot par jate hain. Randomized MAC wale phones ka brand/OUI nahi aata.

---

## 11. FLASH KE BAAD CHECKLIST (agar dobara full flash ho)

Export Config pehle (file Downloads mein) -> flash -> admin login (`Sarfraz`/`admin`) -> Import Config (file chun kar) -> WiFi/admin password badlo -> upstream WiFi (2.4 GHz) -> FW version dekho -> baqi tests. Labels import mein nahi aate, dobara dene honge.
Normal update ke liye flash ki zaroorat nahi: admin > Firmware Update > Check for update > Update now.

---

## 12. PEHLA COMMAND NAYE CHAT KE LIYE (read-only, output auto-copy)

```bash
mkdir -p ~/work-tmp
{
cd ~/esp32-wifi-repeater || { echo "REPO FOLDER NAHI MILA"; exit 1; }
git fetch origin --tags 2>&1; echo "fetch exit: $?"
git status -sb
git log -5 --oneline --decorate
echo "=== AHEAD/BEHIND ==="; git rev-list --left-right --count HEAD...@{u} 2>&1
echo "=== STASH ==="; git stash list
HEAD_SHA=$(git rev-parse HEAD)
echo "=== ACTIONS RUN FOR HEAD ==="
curl -sL -m 20 "https://api.github.com/repos/Ibneilyas124/ESP32_Wifi_Repeater/actions/runs?per_page=5" | grep -A10 "$HEAD_SHA" | grep -E '"(display_title|status|conclusion)"' | head -4
echo "=== LATEST RELEASE ==="
curl -sL -m 20 https://api.github.com/repos/Ibneilyas124/ESP32_Wifi_Repeater/releases/latest | grep -E '"(tag_name|published_at)"'
echo "CHECKPOINT: sync check (read-only) chala, kuch likha nahi"
} 2>&1 | tee ~/work-tmp/last_out.txt
termux-clipboard-set < ~/work-tmp/last_out.txt && echo "COPIED OK" || echo "COPY FAILED"
```

**Edit-script ka standard dhancha** (Claude har baar yahi use kare): `edit.js` (anchors count==1) -> admin.html ke `<script>` blocks `~/work-tmp/adm_N.js` mein nikaal kar `node --check` -> fail par `git checkout --` -> `git diff --stat` -> HANDOVER.md mein line -> `git add -A && git commit && git push` -> Actions wait loop (30 try x 20 sec, `BUILD GREEN (verified)` / `BUILD FAILED`) -> CHECKPOINT.

**Update ka device par tareeqa:** Admin > Advanced > Firmware Update > "Check for update" > "Update now" (~30 sec, reconnect) > dobara Check: "Up to date (build #N)".

**Aakhri CHECKPOINT:** "Router click panel + grey dot reboot push, device par perfect (Sarfraz). Online update, diagnosis fix, Android 11, device label, backup file, reset+esp_wifi_restore, red offline sab device par confirm. Khule: subnet overlap fix, hardware reset check, rollback test, docs update. Naye commits ka Actions green API se unverified."

- 2026-11-13: Failed admin login log: ghalat password par IP/MAC/naam/password/waqt NVS key "flog" mein (max 20, 24h auto-expiry, flush >=30s gap), /api/status mein fail_log, admin > System > Failed Login Attempts. Config export mein shamil nahi. Device test baqi.
