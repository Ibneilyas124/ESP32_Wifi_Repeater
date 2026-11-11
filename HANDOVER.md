# HANDOVER - ESP32 WiFi Repeater (Sarfraz Qureshi) - naye chat ke liye
## Claude ke liye rules (Sarfraz ki pasand)
- Jawab Roman Urdu mein. Termux commands ek ek block mein, code block with language tag (bash/html), explanation code block ke bahar
- Sarfraz ke paas LAPTOP NAHI hai, sirf phone (Termux). ESP32 bhi abhi free nahi (jab free ho tab flash). Laptop/device wale kaam pending list mein hain
- Har read-only command ka output auto-copy: `{ ... ; } 2>&1 | tee ~/work-tmp/last_out.txt` phir `termux-clipboard-set < ~/work-tmp/last_out.txt && echo "COPIED OK" || echo "COPY FAILED"`; checkpoint (echo "CHECKPOINT: ...") usi block mein; secrets ho to copy na karo
- Har step ke baad HANDOVER.md update karna (command ke andar hi merge)
- GitHub Actions build GREEN hone tak agla phase shuru nahi karna (Sarfraz "Greened successfully" likhta hai)
- Sabaq: push se pehle `git pull --rebase`; push ka exit code check karna (git push | tail se ghalat result aata hai); force push kabhi nahi; fetch ke baghair "sync hai" mat kehna
- Sabaq: file ka jo hissa dekha nahi us par edit na likhna, pehle read-only dump mangna. Edit scripts mein har anchor ka aik hi baar milna check karo, warna ABORT (kuch na likho). Pehle se hua ho to bhi ABORT
- Checkpoint hamesha ready: Sarfraz ki limit kisi bhi waqt khatam ho sakti hai. Har step ke command mein HANDOVER.md update + commit + push shamil ho, aur 'Abhi chal raha kaam' line (neeche PENDING 9) hamesha current rakho
- Time kam se kam: jo kuch parhna ho woh aik hi bare command mein mango (alag alag commands nahi); checkpoint/handover mein bhi yehi rule
- Sabaq: apni ghalti turant maan lo (misal: maine ghalat kaha tha ke partitions.csv mein 2 OTA slots hain, asal mein sirf factory tha, ab theek kiya)

## Project
- Repo: https://github.com/Ibneilyas124/esp32-wifi-repeater (public); Termux folder: ~/esp32-wifi-repeater
- ESP-IDF v5.2, ESP32 DevKit V1 4MB, NAT/NAPT repeater; firmware GitHub Actions (.github/workflows/build.yml) se build, har push par naya release build-N
- Files: main/main.c, admin.html, login.html, tools.html, oui_table.h (generated), repeater_lwip_hooks.c/.h, CMakeLists.txt, partitions.csv, sdkconfig.defaults; docs: README.md, FEATURES.md, LEARN_TO_EDIT.md, HANDOVER.md, FLASH_AND_TEST.md; dev-tests/ui_test_admin.js, ui_test_tools.js
- chat.html hat chuki (commit 49ccf40). Purani backups archive: ~/work-tmp/old_backups_20261109_1742.tar.gz
- Guest mode jaisa hai waisa hi: sab connect + admin page tak pahunch, internet sirf whitelisted ko
- FW_VERSION "2.1.0" (main.c line ~53); status API "fw" se admin top bar mein dikhta hai

## DONE (sab Actions builds GREEN)
- Huawei HG8546M style redesign (SIRF design; Huawei logo nahi, apna naam "IBN E ILYAS Technologies"): login.html, admin.html (topology Home, System Info / Advanced Configuration menu, left icon nav), tools.html (neeli top bar, left nav buttons tb-an/de/sp/dg, safed cards, script jyun ka tyun, 36 ids). Teeno ka viewport width=1000 (Android aur desktop par aik jaisi shakal), body min-width 1000px
- Connected Devices: DHCP option 12 hostname capture (main.c dhcp_sniff, sirf printable ASCII, 31 chars; el() textContent istemal karta hai, XSS ka khatra nahi); naam ki tarteeb: whitelist naam, phir DHCP hostname, phir MAC
- Phone brand/model/OS (build GREEN, device par test baqi): main.c http_sniff (port 80 request ka User-Agent, MAC se), ua_capture (/, /admin, /tools, /api/status par UA, peer IP se), dhcp_sniff mein option 60 (android-dhcp-N = Android N, MSFT = Windows), ua_parse, model_table.h (Google supported_devices.csv se, sab mashhoor brands; tools/gen_model_table.py), brand_from_model; devinfo_get MAC wali entry ko tarjeeh deta hai, IP wali baad mein; clients JSON mein brand/model/os; admin.html Connected Devices mein extra line. Sirf RAM, 16 entries. Limits: naya Chrome UA mein model nahi deta (K ignore), iPhone ka model nahi milta, model tab dikhta hai jab phone ne aik HTTP request bheji ho
- Vendor (OUI): main/oui_table.h (IEEE MA-L se, 8925 entries, 26 companies, ~44KB flash), main.c oui_vendor_lookup, clients JSON mein "vendor", admin.html MAC ke saath (company); locally-administered/random MAC par "Private MAC"
- OTA part 1: partitions.csv = nvs 0x9000 (0x6000), phy_init 0xf000, otadata 0x10000 (0x2000), ota_0 0x20000 (0x1E0000), ota_1 0x200000 (0x1E0000); main/CMakeLists.txt mein app_update; workflow merge_bin ab app 0x20000 par
- OTA part 2: CONFIG_APP_ROLLBACK_ENABLE=y (sdkconfig.defaults); main.c ota_post_handler (/api/ota, login zaroori, size 100000..slot, pehla byte 0xE9, esp_ota_end validate, set_boot_partition, restart); web server start ke baad esp_ota_mark_app_valid_cancel_rollback(); admin.html mein Advanced Configuration ke andar Firmware Update (d-ota, #otafile, #otaStatus, otaUpload, size check 1966080)
- Firmware size (OTA se pehle, build-69): esp32-wifi-repeater.bin 947072 bytes (~925KB), slot 1966080 bytes mein aaram se fit
- dev-tests: ui_test_admin.js naye layout + d-ota checks (3 naye); ui_test_tools.js naya (4 tabs, ids, 1000px overflow, screenshots /tmp/tools_<tab>.png; mock JSON shapes andaze se, sirf "no JS errors" fail ho to pehle mock check karna). Syntax check Termux par pass
- Docs: README mein partition-table/full-flash settings warning, 3-file flash tareeqa, "Web update (OTA)" section; LEARN_TO_EDIT.md naye design ke mutabiq + section 7b Firmware Update; README Defaults sirf aik jagah (lines 13-14), duplicate nahi

## NAYE CHAT: TAZA HAAL (pehle yeh parho)
- Nayi features SAB DONE, Actions GREEN: (1) Connected Devices mein first seen/connected since + New tag, (2) guest Request access (login.html form, /api/request bina login, admin Approve/Reject, MAC server peer IP se nikalta hai), (3) whitelist schedule (entry MAC|Name|limit|HHMM-HHMM, setting tz_offset default 5, SNTP sync se pehle lagu nahi), (5) Website Block List (ap_rx_filter mein UDP 53 query drop, NVS 'blocklist', max 20 domains, /api/blocklist, admin d-block). Feature (4) per-device download count NAHI banega (hook sirf upload dekhta hai)
- Phone brand/model/OS: main/model_table.h (Google supported_devices.csv se, 7850 entries, tools/gen_model_table.py), HTTP User-Agent sniff + DHCP option 60. Firmware size ~1.18MB (slot 1.96MB mein 60%)
- URI handlers ~32, max_uri_handlers 34: naya endpoint jodte waqt limit barhana
- Config Export/Import mein blocklist ab shamil (JSON field "blocklist", comma separated, import par blocklist_parse se validate, buffer 4096; Actions GREEN); whitelist ka 4th field (schedule) whitelist string ke saath export hota hai
- Sab features sirf compile hue hain; ESP32 abhi free nahi, device par KUCH test nahi hua
- Naye chat ka pehla qadam: git fetch + status + log -5, phir poocho Sarfraz kya karna chahta hai. Ideas: koi pending nahi (Telegram alert aur Google Drive backup Sarfraz ne chhor diye, neeche sections). Failover, primary-return, blocklist export/import DONE
- Rule yaad: jo parhna ho aik bare command mein mango; har step mein HANDOVER update+commit+push; edit script mein anchors aik hi baar milen warna ABORT; limit kabhi bhi khatam ho sakti hai

- ESP32 ke baghair jo ho sakta tha SAB ho chuka (features, docs, FLASH_AND_TEST.md). Ab sirf ESP32/laptop wale kaam baqi: FLASH_AND_TEST.md tartib se chalao, nateeje HANDOVER mein likho. Naya feature jodne se pehle device test behtar (6 bade features abhi sirf compile hue hain)

## PENDING (device/laptop mangte hain)
1. Pehli flash (ESP32 free hone par, SIRF aik baar): full-flash image NVS (0x9000) mita deti hai (WiFi, whitelist, password). Settings bachani hon to pehle admin se Export Config karo, phir release se 3 alag files: bootloader.bin 0x1000, partition-table.bin 0x8000, esp32-wifi-repeater.bin 0x20000, aur pehle otadata erase: `esptool.py --chip esp32 --port <PORT> erase_region 0x10000 0x2000`, phir `write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 bootloader.bin 0x8000 partition-table.bin 0x20000 esp32-wifi-repeater.bin`. full-flash-padded-0x0.bin naye table ke saath bhi chalti hai (fresh board ke liye) lekin settings mita deti hai
2. Android se alag offsets par flash ka tareeqa abhi tay nahi hua (Android flasher full-flash ke liye tha, compression OFF zaroori); jab flash karna ho to dekhna
3. OTA ka asal test: agla green build ki esp32-wifi-repeater.bin (full-flash NAHI) admin se Firmware Update se upload; ~30 sec mein reconnect; rollback bhi parakhna
4. Laptop par ui_test_admin.js aur ui_test_tools.js: NODE_PATH=<global node_modules> node dev-tests/ui_test_admin.js (Playwright + Chromium chahiye), phir FAIL lines bhejna. Mumkin fail: admin "arrow icon" check (summary ::after), "no element overflows" (design 1000px ka, admin test 380px par chalta hai)
5. /tools page phone par nazar-e-sani (chart, pill, tags rang naye halke background par); script ke andar ke inline dark rang CSS se override hain (#netBanner h2)
6. One-Click Diagnosis menu item admin mein DONE (d-diag, build-115 GREEN a60d0b8; device par asli test baqi); FEATURES.md update DONE (vendor, hostname, OTA, design, flash note; OTA asal test baqi)
7. Naye features ki soch: jo Sarfraz kahe (poochna)
8. Phone model/OS ka asal device test: kaun se phones par brand/model/OS aaya; phir MODEL_MAP un brands (Xiaomi, Infinix, Tecno...) ke hisaab se bharna. Sarfraz ke phones ka brand/browser abhi nahi pata (poochna)
9. Abhi chal raha kaam: ESP32 BUSY hai, isliye ONLINE UPDATE ka device test baad mein (ZAROOR karna hai: build green hone ke baad manual upload, phir Check for update, phir do builds ke darmiyan Update now, rollback bhi). Naya kaam: phone MODEL detection behtar karna (company aa jati hai, model nahi, misal Motorola moto one 5G). Purani line: online update (admin Firmware Update: Check for update / Update now, GitHub releases/latest se build-N, esp_https_ota, FW_BUILD build_num.h workflow se) push hua, Actions green ka intezar; device test baqi (pehli baar manual upload, phir do builds ke darmiyan Update now, rollback bhi). Pehle: Device par test DONE (2026-11-10): guest request Home par aati hai aur Approve par MAC whitelist mein jata hai; httpd lru_purge se admin page hang khatam. Guest ko ab saaf error dikhta hai, debug code (peer_mac) sirf serial log mein. build-118 (94106ff) tak sab GREEN. One-Click Diagnosis admin mein DONE (build-115 GREEN). Sab Actions GREEN (docs commit samet). DOCS DONE (doc-only, firmware par asar nahi): README v11 + layout, FEATURES blocklist line, LEARN_TO_EDIT section 9, nayi FLASH_AND_TEST.md (flash + har feature ka test checklist: ESP32 free hone par isi se chalo). flash.sh ka app offset 0x20000 kar diya (DONE). flash.sh ke echo login/WiFi details theek kiye (WiFi Sarfraz/Sarfraz1, login Sarfraz/admin, main.c se confirm) aur otadata comment dala (DONE). FAILOVER (backup upstream) aur PRIMARY-RETURN timer DONE, Actions GREEN. Details: main.c sta_apply_network/s_on_backup/STA_FAILOVER_FAILS=5, NVS sta_ssid2/sta_pass2, /api/wifi ssid2+password2 (password2 sirf bheja ho to likha jata hai), status on_backup+sta_ssid2, admin Backup SSID fields + stat 'Upstream network', export/import shamil. Primary wapsi: button_task har ~5 min (tick%3000) primary_return_task banata hai (stack 4096, scan ssid filter se, primary dikhe to switch back; s_pr_busy guard, s_scan_state share). Limits: hidden primary SSID scan mein nahi milta to wapsi nahi; primary dikhe par password ghalat ho to har 5 min ~10-20 sec ka outage; static IP setting dono networks par ek hi lagti hai; device par test baqi. Config export/import mein blocklist DONE, Actions GREEN. Features 1,2,3,5 DONE, Actions GREEN (build-96 tak). Feature 4 (per-device download count) NAHI banega. /tools#dg hash link DONE. Baqi sirf device/laptop wale kaam (PENDING 1-5, 8). Naye chat mein: git fetch + status + log -5, phir Sarfraz se poocho kya karna hai (ideas: koi pending nahi; Telegram alert aur Google Drive backup chhor diye).
10. ESP32 free hone par FLASH_AND_TEST.md ke mutabiq: pehli flash, OTA, config export/import (blocklist), first seen, request access, schedule, block list, failover + primary-return, phone model/OS. flash.sh offset aur echo details DONE; pehle se pata ho to PENDING 2 se jodna

## Huawei design (screenshots ka khulasa)
- Upar halki neeli bar (~#5bb4ef): left model naam, right links; Left: patli icon column, active item neela + chhota teer; sub-menu grey column; Content: halka neela-safed background (#f5f8fc), bara title + patli line, orange description text, safed tables grey header, safed buttons border ke saath
- Login: neela gradient, bara Welcome text, safed Log In button, neeche copyright patti

## admin.html dhancha (ids na toorna)
- Menu: go(id) aik waqt mein aik section; SECS = home:[p-home], sys:[p-info,d-usage,d-speed,d-stab], adv:[d-wifi,d-wl,d-block,d-adv,d-backup,d-ota,d-danger] (admin.html line 301 se confirm); Home par topology click (toggleDevs) se d-clients
- Sections: statGrid, d-clients (#clientCount, #clientsTbl), d-wl (#wlList, #newmac, #newname, #newlimit, addMac), d-wifi (#sta_ssid, #sta_pass, saveWifi), d-usage (#overallUsage, #usageBody), d-speed (#shChart), d-stab (#stabBody), d-adv (#advGroups, fillDefaults, saveSettings), d-backup (#importbox, exportCfg, importCfg), d-ota (otaUpload), d-danger (Reboot, Factory Reset); header #fwLine, Logout (doLogout)
- API: /api/status /api/whitelist /api/settings /api/wifi /api/usage /api/speedhistory /api/kick /api/reboot /api/factory_reset /api/config/export /api/config/import /api/logout /api/me /api/ota /api/blocklist /api/request (bina login) /api/requests (admin); tools: /api/scan(/start) /api/deauth /api/ping /api/speed /api/upspeed /api/diag(/start); URI handlers ~32, max_uri_handlers 34 (naya endpoint jodte waqt limit barhao)
- Naye ids/functions (features 1,2,3,5): New tag (first seen), #newsched (whitelist schedule), reqList/loadRequests (guest Request access, Approve/Reject), d-block (#blockText, loadBlock, saveBlock); login.html mein Request access form
- main.c: require_session_api(req) har login-wala handler ki pehli line; send_result, restart_soon helpers
- Firmware audit (jari): logout par login lazmi kiya; speedhistory public hi rahegi (tools.html istemal karta hai); OTA, auth, NVS, dns_blocked, dhcp_sniff, ua_parse bounds theek. Packet limiter ka bucket cap kam az kam 1600 kiya. BAQI: ap_rx_filter hot path mein whitelist/NVS/setting har packet par parhna (cache ka sawal), whitelist_contains mein malloc fail par fail-open, sta_rx_counter, scan/deauth/wifi_init parhna.
- Audit: sta_rx_counter cap kam az kam 1600; whitelist_contains / whitelist_get_limit se strdup hata diya (stack list par strtok, malloc fail par fail-open khatam). BAQI: nvs_get_string mein RAM cache nahi (har packet par kai NVS reads): ESP32 par speed test ke baad hi cache banana (whitelist + overall_up/down + limit, badlav par refresh). Pehle device par naap lo.
- Audit: config_import, sta_apply_network, primary_return_task parhe: theek. Import mein SSID <=32 / password <=64 ki hadd lagayi (pehle lamba SSID chupke se khali ban jata tha). Nukte (ESP32 par dekhna): primary_return_task ka clear_ap_list admin scan ke nateeje saaf kar sakta hai; sta_apply_network ke static buffers do task istemal karte hain. BAQI audit: wifi_post, scan, deauth, request_post, requests_post, kick, blocklist_post, validate_setting, wifi_init.
- Audit: import mein upstream/backup password sirf khali ya 8-63 (pehle 1-7 aur 64 chale jate the, wifi_init ESP_ERROR_CHECK se boot loop ka khatra); wifi_init mein STA set_config ab abort nahi karta, sirf warning. BAQI audit: button_task ka baqi, status_get, config_export, settings_get/post, app_main. ESP32 par dekhna: WiFi task stack (ap_rx_filter mein 1KB whitelist buffer + NVS), NVS har packet par.
- Audit 16b: speed_get, root/admin/tools/me handlers, repeater_lwip_hooks.c theek. Firmware audit (code parhna) lagbhag mukammal: 4 asli ghaltiyan theek ki (logout auth, rate limiter cap up/down, import SSID/password hadd, import ghalat password se boot loop). BAQI: (1) qos_manager.c/.h HATA DIYE (CMakeLists SRCS mein tha hi nahi, kahin call nahi; audit 16c DONE); (2) scan_start/diag_start mein xTaskCreate ka result check DONE (fail par scan state 2 = error, diag state 0 = idle); (3) ESP32 par naapna: WiFi task stack (ap_rx_filter mein 1KB buffer + NVS), button_task stack 3072, NVS har packet par (cache sirf naap ke baad); (4) [DEKHA audit 16c: handler mein sta_apply_network + 1s vTaskDelay default event task ko rokte hain, badla nahi, ESP32 par dekhna] event task mein DISCONNECTED par 1s delay; (5) FLASH_AND_TEST.md ke mutabiq asli test aur laptop par ui_test_admin.js / ui_test_tools.js.

## Audit 16c (2026-11-10)
- Sync check: fetch ke baad main == origin/main (de150af), latest green release build-113; purani line 'build-96 tak' purani hai
- Dead code hata diya: main/qos_manager.c/.h (CMakeLists SRCS sirf main.c, kahin call nahi, sirf HANDOVER mein zikr tha)
- scan_start/diag_start: xTaskCreate fail par scan state 2 (error, tools.html scan 'error' dikhata hai), diag state 0 (idle); tools.html diag polling dekha: idle par 25 sec baad 'Timed out.', atkna nahi
- URI handlers verify: uris[] mein 32 entries (aik loop mein register), max_uri_handlers 34 = 2 spare; naya endpoint jode to limit barhao
- WIFI_EVENT_STA_DISCONNECTED dekha: sta_apply_network + 1s vTaskDelay default event task mein; flapping par AP join events late ho sakte hain; badla nahi, ESP32 par dekhna (idea: esp_timer one-shot se reconnect)
- BAQI audit: (3) ESP32 par naapna (WiFi task stack, button_task stack 3072, NVS har packet par); (5) FLASH_AND_TEST.md se asli test + laptop UI tests

## One-Click Diagnosis in admin (2026-11-10)
- Audit 16c Actions GREEN (build-114, 1f6c183). tools.html diag polling dekha: state idle par 25 sec baad 'Timed out.', permanent atkna nahi, tools.html badla nahi
- admin.html: naya section d-diag (#dgAOut, #dgABtn, runDiagAdmin; SECS.sys mein jora), System Info subnav mein 'One-Click Diagnosis' link; left menu ka One-Click Diagnosis ab admin ke andar khulta hai aur diagnosis khud chalta hai (pehle /tools#dg, woh link tools.html mein ab bhi chalta hai)
- runDiagAdmin wahi /api/diag/start + /api/diag istemal karta hai, idle par 'could not start' dikhata hai, sab text textContent/createTextNode se; main.c badla nahi
- Housekeeping DONE (2026-11-10): ui_test_admin.js ki dono section lists mein d-diag aur d-block jore (sirf section-show checks); FEATURES.md Diagnose line, FLASH_AND_TEST.md (menu ab admin ke andar), LEARN_TO_EDIT.md section 8b; PENDING 6 DONE; build-115 (a60d0b8) GREEN, admin/tools/login ke inline scripts node --check OK
- BAQI: laptop par ui_test_admin.js chalana (agar naye d-block/d-diag section checks par FAIL aaye to pehle mock check karna, mock ke endpoints maine dekhe nahi); device par One-Click Diagnosis aik baar chala kar natija dekhna

## Telegram alert: CHHOR DIYA (2026-11-10)
- Sarfraz ne Telegram alert ka idea chhor diya (design par baat hui thi: HTTPS/TLS bundle, bot token NVS mein, heap ka sawal); jab tak woh khud na kahe dobara mat sujhao. Purani lines 'ideas: Telegram alert, ...' is note se override hain
- ESP32 ke baghair ka kaam lagbhag khatam; baqi: ESP32 free hone par FLASH_AND_TEST.md, laptop par UI tests (ui_test_admin.js, ui_test_tools.js)

## Google Drive backup: CHHOR DIYA (2026-11-10)
- Sarfraz ne Google Drive backup (OAuth wala bara kaam) bhi chhor diya; jab tak woh khud na kahe dobara mat sujhao. Purani lines 'Google Drive backup' / 'ideas: ...' is note se override hain. Backup ka tareeqa wahi rahega: admin se Export Config / Import Config
- Ab koi naya feature pending nahi (PENDING 7: naye features ki soch, jo Sarfraz kahe wahi). Baqi sirf device/laptop kaam: FLASH_AND_TEST.md (ESP32 free hone par), laptop par ui_test_admin.js / ui_test_tools.js
- Docs safai (2026-11-10): HANDOVER ki purani 'ideas' lines (NAYE CHAT section aur PENDING 9), README v7 aur FEATURES 'Known limitations' se Google Drive/Telegram ka zikr saaf; ab docs mein Drive backup 'not planned' likha hai (backup = Export Config / Import Config)

- 2026-11-10: guest access requests ab admin Home (p-home) par dikhti hain (reqList d-wl se nikal kar); approve error alert se. Device par test baqi.

- 2026-11-10: guest requests admin Home par (b655129). Admin page hang ka andaza: httpd sockets full (max_open_sockets 7, lru_purge off); fix lru_purge_enable=true, device par test baqi.

- 2026-11-10: httpd lru_purge fix device par kaam kiya (admin page hang khatam). Guest request: "could not identify your device" aaya; peer_mac mein IPv6-mapped handling + error mein code/family/stations; device test baqi.

- 2026-11-10: device test PASS: guest access request (Home par dikhti, approve se whitelist) aur admin hang fix. peer_mac IPv6-mapped handling rakhi; guest error message saaf, debug code ESP_LOGW mein. Agla: FLASH_AND_TEST.md ke baqi tests (OTA, schedule, blocklist, failover, config export/import).

- 2026-11-10: online update banaya: main.c upd_task/upd_check/upd_install, /api/update (GET) + /api/update/start (POST), max_uri_handlers 36, admin d-ota mein Check/Update now, FW_BUILD main/build_num.h (default 0, workflow run_number se overwrite), sdkconfig TLS bundle, CMake REQUIRES esp_http_client esp_https_ota esp-tls mbedtls heap. Heap hadd 60000/24000 andaza hai, device par tune. Sirf compile; device test baqi.

- 2026-11-10: online update par "clock not synced yet" aaya (SNTP boot par sirf aik baar start hota hai). Fix: upd_sync_clock() (SNTP restart, pool.ntp.org phir time.google.com, 12 sec har aik). Sirf compile; device test baqi.

- 2026-11-10: NTP (UDP) dono servers par fail hua (upstream par band). Fix: upd_http_time() HTTP Date header (connectivitycheck.gstatic.com, phir google.com) se settimeofday, NTP fallback. Error mein wajah (s_time_err). Sirf compile; device test baqi.

- 2026-11-10: build 2ca2e95 fail (format-truncation: clock error message 76-115 bytes, s_upd_msg[96]). Fix: s_upd_msg[160]. Sirf compile; device test baqi.

- 2026-11-10: Sarfraz ne kaha: ESP32 busy hai, online update (Check/Update now) ka test baad mein lekin zaroor. Naya kaam: Connected Devices mein company (OUI) sahi aa rahi hai lekin model nahi (Motorola); model detection par kaam karna hai. Sarfraz ko yeh pasand aaya ke branded routers sirf MAC dikhate hain jabke repeater company tak pahunchta hai.

- 2026-11-10: phone model detection: UA se model khali (Chrome "K") to DHCP hostname se model (host_model_guess: moto/galaxy/redmi/pixel/infinix/tecno... prefix, - ko space), clients JSON mein; brand_from_model mein moto->Motorola, Galaxy->Samsung. Sirf compile; device test baqi (ESP32 busy). Dekhna: Motorola phone ka hostname asal mein model hai ya android-xxxx.

## Update 2026-11-11
- build-129 (0b1a9dd) Actions API se GREEN verified.
- Online update: HTTP clock fail ESP_ERR_HTTP_CONNECT; error ab DNS/connect alag batata hai + STA fallback DNS 8.8.8.8. Device test baqi.
- Online update: HTTP 301 (repo rename) fix: UPD_BASE_URL ab ESP32_Wifi_Repeater. Clock sync ab theek (TLS GitHub tak pahuncha). Diagnosis "Timed out" abhi open.
- Online update CHECK device par PASS: "Up to date (build #131)". Update now + rollback test baqi. Diagnosis "Timed out" open (dump ke baad fix).
