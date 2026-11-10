# FLASH AND TEST - ESP32 free hone par (Sarfraz ke liye)

Yeh checklist us waqt ke liye hai jab ESP32 free ho. Ab tak sab features sirf
compile hue hain (GitHub Actions GREEN), device par aik bhi test nahi hua.
Har test ke baad nateeja HANDOVER.md mein likhna (pass / fail / kya dikha).

## 0. Flash se pehle
- Agar repeater pehle se chal raha hai aur settings bachani hain: admin se **Export Config** karke text kahin save karo (WiFi, whitelist, password sab mita diya jata hai agar full-flash image use ho)
- Latest Release se files lo: `bootloader.bin`, `partition-table.bin`, `esp32-wifi-repeater.bin` (aur fresh board ke liye `full-flash-padded-0x0.bin`)

## 1. Pehli flash (SIRF aik baar, naya partition table)
Settings bachani hon (3 alag files, app 0x20000 par):

```bash
python3 -m esptool --chip esp32 --port /dev/ttyUSB0 erase_region 0x10000 0x2000
python3 -m esptool --chip esp32 --port /dev/ttyUSB0 write_flash -z \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 bootloader.bin 0x8000 partition-table.bin 0x20000 esp32-wifi-repeater.bin
```

Fresh board (settings mit jayengi): `full-flash-padded-0x0.bin` offset 0x0 par.
Android flasher se alag offsets ka tareeqa abhi tay nahi hua; full-flash ke liye compression OFF zaroori hai.
`flash.sh` mein app ka offset 0x20000 hona chahiye (purana 0x10000 ghalat hai), istemal se pehle check karo.

Dekhna: boot ke baad "AP up" log aaye, phone ko WiFi `Sarfraz` / `Sarfraz1` mile, `http://192.168.4.1` khule, login `Sarfraz` / `admin` chale.

## 2. Bunyadi check
- Upstream WiFi mein router ka SSID/password save karo, reboot ke baad Home par Upstream IP dikhe aur internet chale
- Connected Devices mein device ka naam / MAC / company dikhe (random MAC par "Private MAC")
- Teeno pages (login, admin, /tools) phone par aik jaisi shakal mein khulen, koi JS error na ho
- Admin menu ka "One-Click Diagnosis" admin ke andar hi khule aur diagnosis khud chalaye (steps: upstream, gateway, internet, DNS); /tools#dg bhi pehle ki tarah chalna chahiye

## 3. OTA (web se update)
1. Agla green build ki `esp32-wifi-repeater.bin` lo (full-flash NAHI)
2. Advanced Configuration -> Firmware Update -> file chuno -> Upload and Install
3. Dekhna: ~30 sec mein repeater restart ho, dobara connect ho, FW line theek dikhe
4. Rollback ka test optional hai (aisi image jis ka web server na chale); zaroorat par hi

## 4. Config Export / Import (blocklist samet)
1. Block List mein 2 domains likho, Save
2. Export Config: JSON mein `"blocklist"` field dikhe
3. Block List badal do, phir purana JSON Import karo; reboot ke baad list wapas wohi aaye
4. Purani export file (bina blocklist field) import karo: blocklist jaisi thi waisi rahe

## 5. Naye features
- **First seen / New tag**: naya device join kare to Connected Devices mein "New" tag aur connected since dikhe
- **Request access**: guest phone login page par naam likh kar request bheje; admin MAC Whitelist ke upar request dekhe, Approve kare; phir us phone ko internet mile (MAC kabhi type nahi karna parta)
- **Whitelist schedule**: pehle internet se clock sync hone do (SNTP). Aik whitelisted device ko aisi window do jo abhi se bahar ho (maslan HHMM-HHMM): us par internet band, local access chale. Window ke andar aane par internet chale. tz_offset 5 (Pakistan)
- **Website Block List**: domain add karo; phone par Private DNS band rakho; sites khulna band, subdomain bhi band; list se hatao to khul jaye
- **Phone model / OS**: kaun se phones par brand/model/OS aaya, note karo (iPhone par sirf "Apple . iPhone"; naya Chrome model nahi deta). Jo brands khali rahen unke naam HANDOVER mein likho

## 6. Failover (backup upstream)
1. Upstream WiFi mein Backup SSID/password do (sahi router), Save
2. Primary ka password jaan kar ghalat kar do (ya primary router band karo)
3. Dekhna: ~10-20 sec mein serial log mein "Upstream failover: switching to BACKUP"; Home par "Upstream network: Backup"; internet chale
4. Primary router wapas on karo: ~5 min mein log mein "Primary upstream visible again - switching back from BACKUP"; status "Primary"
5. Dhyan: scan ke waqt clients ko 1-2 sec ka jhatka mumkin hai; "Remote-management IP (static)" set ho to woh dono networks par lagti hai (failover ke saath khali rakho)

## 7. Laptop par UI tests
```bash
NODE_PATH=<global node_modules> node dev-tests/ui_test_admin.js
NODE_PATH=<global node_modules> node dev-tests/ui_test_tools.js
```
FAIL lines HANDOVER ya naye chat mein bhejna. Mumkin fail: admin "arrow icon", "no element overflows" (design 1000px ka, test 380px par chalta hai), tools ka "no JS errors" (mock JSON andaze se bane hain).
