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
- Files: main/main.c, admin.html, login.html, tools.html, oui_table.h (generated), qos_manager.c/.h, repeater_lwip_hooks.c/.h, CMakeLists.txt, partitions.csv, sdkconfig.defaults; docs: README.md, FEATURES.md, LEARN_TO_EDIT.md, HANDOVER.md; dev-tests/ui_test_admin.js, ui_test_tools.js
- chat.html hat chuki (commit 49ccf40). Purani backups archive: ~/work-tmp/old_backups_20261109_1742.tar.gz
- Guest mode jaisa hai waisa hi: sab connect + admin page tak pahunch, internet sirf whitelisted ko
- FW_VERSION "2.1.0" (main.c line ~53); status API "fw" se admin top bar mein dikhta hai

## DONE (sab Actions builds GREEN)
- Huawei HG8546M style redesign (SIRF design; Huawei logo nahi, apna naam "IBN E ILYAS Technologies"): login.html, admin.html (topology Home, System Info / Advanced Configuration menu, left icon nav), tools.html (neeli top bar, left nav buttons tb-an/de/sp/dg, safed cards, script jyun ka tyun, 36 ids). Teeno ka viewport width=1000 (Android aur desktop par aik jaisi shakal), body min-width 1000px
- Connected Devices: DHCP option 12 hostname capture (main.c dhcp_sniff, sirf printable ASCII, 31 chars; el() textContent istemal karta hai, XSS ka khatra nahi); naam ki tarteeb: whitelist naam, phir DHCP hostname, phir MAC
- Phone brand/model/OS (build GREEN, device par test baqi): main.c http_sniff (port 80 request ka User-Agent, MAC se), ua_capture (/, /admin, /tools, /api/status par UA, peer IP se), dhcp_sniff mein option 60 (android-dhcp-N = Android N, MSFT = Windows), ua_parse, MODEL_MAP (abhi sirf Samsung SM-xxxx ke naam), brand_from_model; devinfo_get MAC wali entry ko tarjeeh deta hai, IP wali baad mein; clients JSON mein brand/model/os; admin.html Connected Devices mein extra line. Sirf RAM, 16 entries. Limits: naya Chrome UA mein model nahi deta (K ignore), iPhone ka model nahi milta, model tab dikhta hai jab phone ne aik HTTP request bheji ho
- Vendor (OUI): main/oui_table.h (IEEE MA-L se, 8925 entries, 26 companies, ~44KB flash), main.c oui_vendor_lookup, clients JSON mein "vendor", admin.html MAC ke saath (company); locally-administered/random MAC par "Private MAC"
- OTA part 1: partitions.csv = nvs 0x9000 (0x6000), phy_init 0xf000, otadata 0x10000 (0x2000), ota_0 0x20000 (0x1E0000), ota_1 0x200000 (0x1E0000); main/CMakeLists.txt mein app_update; workflow merge_bin ab app 0x20000 par
- OTA part 2: CONFIG_APP_ROLLBACK_ENABLE=y (sdkconfig.defaults); main.c ota_post_handler (/api/ota, login zaroori, size 100000..slot, pehla byte 0xE9, esp_ota_end validate, set_boot_partition, restart); web server start ke baad esp_ota_mark_app_valid_cancel_rollback(); admin.html mein Advanced Configuration ke andar Firmware Update (d-ota, #otafile, #otaStatus, otaUpload, size check 1966080)
- Firmware size (OTA se pehle, build-69): esp32-wifi-repeater.bin 947072 bytes (~925KB), slot 1966080 bytes mein aaram se fit
- dev-tests: ui_test_admin.js naye layout + d-ota checks (3 naye); ui_test_tools.js naya (4 tabs, ids, 1000px overflow, screenshots /tmp/tools_<tab>.png; mock JSON shapes andaze se, sirf "no JS errors" fail ho to pehle mock check karna). Syntax check Termux par pass
- Docs: README mein partition-table/full-flash settings warning, 3-file flash tareeqa, "Web update (OTA)" section; LEARN_TO_EDIT.md naye design ke mutabiq + section 7b Firmware Update; README Defaults sirf aik jagah (lines 13-14), duplicate nahi

## PENDING (device/laptop mangte hain)
1. Pehli flash (ESP32 free hone par, SIRF aik baar): full-flash image NVS (0x9000) mita deti hai (WiFi, whitelist, password). Settings bachani hon to pehle admin se Export Config karo, phir release se 3 alag files: bootloader.bin 0x1000, partition-table.bin 0x8000, esp32-wifi-repeater.bin 0x20000, aur pehle otadata erase: `esptool.py --chip esp32 --port <PORT> erase_region 0x10000 0x2000`, phir `write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 bootloader.bin 0x8000 partition-table.bin 0x20000 esp32-wifi-repeater.bin`. full-flash-padded-0x0.bin naye table ke saath bhi chalti hai (fresh board ke liye) lekin settings mita deti hai
2. Android se alag offsets par flash ka tareeqa abhi tay nahi hua (Android flasher full-flash ke liye tha, compression OFF zaroori); jab flash karna ho to dekhna
3. OTA ka asal test: agla green build ki esp32-wifi-repeater.bin (full-flash NAHI) admin se Firmware Update se upload; ~30 sec mein reconnect; rollback bhi parakhna
4. Laptop par ui_test_admin.js aur ui_test_tools.js: NODE_PATH=<global node_modules> node dev-tests/ui_test_admin.js (Playwright + Chromium chahiye), phir FAIL lines bhejna. Mumkin fail: admin "arrow icon" check (summary ::after), "no element overflows" (design 1000px ka, admin test 380px par chalta hai)
5. /tools page phone par nazar-e-sani (chart, pill, tags rang naye halke background par); script ke andar ke inline dark rang CSS se override hain (#netBanner h2)
6. One-Click Diagnosis menu item admin mein (Diagnose abhi tools.html mein hai); FEATURES.md update DONE (vendor, hostname, OTA, design, flash note; OTA asal test baqi)
7. Naye features ki soch: jo Sarfraz kahe (poochna)
8. Phone model/OS ka asal device test: kaun se phones par brand/model/OS aaya; phir MODEL_MAP un brands (Xiaomi, Infinix, Tecno...) ke hisaab se bharna. Sarfraz ke phones ka brand/browser abhi nahi pata (poochna)
9. Abhi chal raha kaam: (DONE: /tools#dg hash link GREEN; ui_test_tools.js mein hash tests + diag mock likhe, node --check Termux par; ASAL TEST laptop par baqi, PENDING 4 mein shamil). Agla kaam: PENDING 5 (/tools inline dark rang review) ya MODEL_MAP (Sarfraz ke phones ka brand batana baqi) One-Click Diagnosis menu item admin.html mein PEHLE SE hai, ab href=/tools#dg aur tools.html hash se Diagnose tab kholta + runDiag() chalata hai. Purani note: One-Click Diagnosis menu item admin.html mein PEHLE SE hai (line ~114, href=/tools, ids/SECS na chhedo). Baqi kaam: link /tools#dg ho aur tools.html start-up par location.hash 'dg' ho to showTab('dg') chale (script logic na chhedo, sirf aik chhoti line); ui_test_tools.js mein hash test. Abhi koi edit nahi hui

## Huawei design (screenshots ka khulasa)
- Upar halki neeli bar (~#5bb4ef): left model naam, right links; Left: patli icon column, active item neela + chhota teer; sub-menu grey column; Content: halka neela-safed background (#f5f8fc), bara title + patli line, orange description text, safed tables grey header, safed buttons border ke saath
- Login: neela gradient, bara Welcome text, safed Log In button, neeche copyright patti

## admin.html dhancha (ids na toorna)
- Menu: go(id) aik waqt mein aik section; SECS = home:[p-home], sys:[p-info,d-usage,d-speed,d-stab], adv:[d-wifi,d-wl,d-adv,d-backup,d-ota,d-danger]; Home par topology click (toggleDevs) se d-clients
- Sections: statGrid, d-clients (#clientCount, #clientsTbl), d-wl (#wlList, #newmac, #newname, #newlimit, addMac), d-wifi (#sta_ssid, #sta_pass, saveWifi), d-usage (#overallUsage, #usageBody), d-speed (#shChart), d-stab (#stabBody), d-adv (#advGroups, fillDefaults, saveSettings), d-backup (#importbox, exportCfg, importCfg), d-ota (otaUpload), d-danger (Reboot, Factory Reset); header #fwLine, Logout (doLogout)
- API: /api/status /api/whitelist /api/settings /api/wifi /api/usage /api/speedhistory /api/kick /api/reboot /api/factory_reset /api/config/export /api/config/import /api/logout /api/me /api/ota; tools: /api/scan(/start) /api/deauth /api/ping /api/speed /api/upspeed /api/diag(/start); uri table mein 26 handlers (limit 30)
- main.c: require_session_api(req) har login-wala handler ki pehli line; send_result, restart_soon helpers
