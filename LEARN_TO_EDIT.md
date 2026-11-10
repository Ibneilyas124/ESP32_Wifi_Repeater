# LEARN TO EDIT — kahan kya milega

Yeh file sirf tumhare liye hai (compile nahi hoti). Isme har wo text/paragraph
ka pata hai jo repeater ki screens par dikhta hai, aur usay GitHub website se
seedha (edit ✏️ icon se) kaise badalna hai. Poori coding ko haath mat lagana —
sirf neeche batayi gayi **quotes ke andar wali cheez** badalna.

Kisi bhi file ko edit karne ka tareeqa hamesha same hai:
1. GitHub par repo kholo → file pe click karo → pencil ✏️ (Edit this file)
2. Neeche di gayi "dhoondo" wali line Ctrl+F (ya phone par "find in page") se
   dhoondo
3. Quotes ke andar wala text apna likha hua text se replace karo (quotes
   `" "` ko mat chhedna, sirf andar wala hissa)
4. Neeche scroll karke **Commit changes** dabao
5. Naya build khud chal jayega (Actions tab), phir naya firmware flash karna

---

## 1. Login page ka heading — "Sarfraz Ibn E Ilyas" / builder ka naam
**File:** `main/login.html`
**Dhoondo:** `This WiFi Repeater was built by<br>Sarfraz Qureshi`
Yahan naam/tagline badal sakte ho. `<br>` ko mat hataana, yeh sirf line-break hai.

## 2. Galat password ka message
**File:** `main/login.html`
**Dhoondo:** `Wrong password`

## 3. About Developer ka poora popup (contact, skills, quote)
**File:** `main/login.html`
**Dhoondo:** `About Developer` (yeh heading milte hi neeche poora box hai)
Andar yeh sab editable hai:
- Naam: `Sarfraz Qureshi`
- Location: `Naushehro Feroz, Sindh, Pakistan`
- Email: `ahmedsarfraz650@gmail.com` (2 jagah — link aur text dono)
- WhatsApp number: `923043478576` (link ke andar) aur `+92 304 3478576` (dikhne wala text)
- GitHub: `Ibneilyas124` (2 jagah)
- Skills wali list: `<li>...</li>` wali 5 lines
- Neeche wala quote: `Build it. Break it. Fix it. Learn from it.`

## 4. Admin page ka heading — "IBN E ILYAS Technologies"
**File:** `main/admin.html`
**Dhoondo:** `IBN E ILYAS Technologies` (upar wali neeli bar mein)

Yeh page **Huawei style** mein hai: upar neeli bar (naam `IBN E ILYAS Technologies`),
left mein icon menu (Home Page / System Info / Advanced Configuration), aur har
section ek `<details class="sec">` block hai jo menu se khulta hai. **Element ids
(jaise `statGrid`, `clientsTbl`, `otafile`) na badalna**, kyunke test aur JS unhi
par chalte hain. Advanced Settings ke andar 4 chhote groups hain (WiFi & Network,
Security & Access, Advanced Network, Bandwidth Limits): group ka naam
`GROUP_LABEL` object mein (`admin.html` ke JS mein) milega, aur kaunsi setting kis
group mein hai woh `main/main.c` ke `SETTINGS[]` table ki aakhri column mein hai.

## 5. Company/business ka naam jo main router mein dikhta hai
Yeh **code mein nahi**, balke ek **setting** hai — dashboard mein khud login
karke Advanced Settings ke andar "Name shown in main router" waali field se
badal sakte ho, reflash ki zaroorat nahi.

## 6. Default AP WiFi naam/password, admin password, DNS, waghera
Yeh bhi sab **settings** hain, code mein nahi — Advanced Settings mein hi
milenge. Agar tumhe sirf **factory-default value** badalni ho (jo naya device
pehli baar boot hone par use karta hai), to:
**File:** `main/main.c`
**Dhoondo:** `static const setting_t SETTINGS[]` — is list ki har line mein
2nd column default value hai (quotes ke andar). Yahan sirf quotes ke andar
wali value badalna, baaki kuch nahi.

## 7. WiFi Tools page ka heading aur tabs (Analyzer/Deauth/Speed/Diagnose)
**File:** `main/tools.html`
**Dhoondo:** `WiFi Tools`, ya tab names jaise `Analyzer`, `Deauth`, `Speed`, `Diagnose`
Naye design mein tabs left wale menu ke buttons hain (`id="tb-an"`, `tb-de`, `tb-sp`, `tb-dg`), aur upar bar mein `IBN E ILYAS Technologies` likha hai. Buttons ke ids aur `onclick="showTab(...)"` na chhedna.

## 7b. Firmware Update (web se, OTA)
**File:** `main/admin.html` (section `d-ota`, JS `otaUpload`) aur `main/main.c` (`ota_post_handler`)
**Dhoondo:** `Firmware Update`
Sirf text badalna ho to `admin.html` mein `Firmware Update` ke aas paas ka matn badlein. Upload ka size check (`1966080`) `main.c` ke slot size (0x1E0000) se mel khana chahiye; `partitions.csv` badle to dono jagah dekhna.

## 8. Konse buttons/labels kahan hain (quick reference)
| Screen par jo dikhta hai | File | Kya dhoondo |
|---|---|---|
| Login form ka "Login" button | `login.html` | `>Login<` |
| "WiFi Tools (no login)" link | `login.html` | `WiFi Tools` |
| Dashboard ka "Save & Reboot" | `admin.html` | `Save &amp; Reboot` |
| "Factory Reset" button/warning | `admin.html` | `Factory Reset` |
| Hardware reset ka instruction | `admin.html` | `hold the BOOT button` |
| "Share this WiFi" card | `admin.html` | `Share this WiFi` |
| Upstream stability ke reason names | `admin.html` | `var REASONS=` |
| "Upload and Install" (firmware update) | `admin.html` | `Upload and Install` |

---

## 8b. One-Click Diagnosis (admin ke andar)

**Dhoondo:** `One-Click Diagnosis`, `d-diag`, ya `runDiagAdmin` - file `main/admin.html`
- Left menu ka link aur System Info ke andar ka link: `go('d-diag')` wali do lines
- Section ka title, description aur "Run diagnosis" button: `<details class="sec" id="d-diag">`
- Natije ke messages (Upstream, Gateway, Internet, DNS): isi section ke script mein `show(j)` function
- `/tools` page ka Diagnose tab alag hai (`main/tools.html`, `showDiag`): aik jagah ka text badlo to doosri jagah bhi badlo

## Zaroori ehtiyaat
- Sirf **do quotes ke beech** wala matn badalna — quotes, commas, semicolons
  (`;`), curly brackets (`{ }`) waisa hi rehne dena.
- Agar kabhi confuse ho ke yeh C code hai ya HTML text, bas itna yaad rakho:
  `main.c` mein sirf **Section 6** ke `SETTINGS[]` table ke andar ki quotes
  chhedni hain, baaki `main.c` na chhedna.
- Koi bhi edit karne ke baad **Actions tab** mein build zaroor dekhna — agar
  laal ❌ aaye to us error ka pehla `error:` wala hissa Claude ko bhej dena.

## 9. Naye features: kahan kya hai
- **Website Block List**: admin.html mein section `d-block` (`#blockText`, `loadBlock`, `saveBlock`). Firmware mein `blocklist_parse` / `dns_blocked` (main.c), NVS key `blocklist`, max 20 domains
- **Request access** (guest): login.html ka form; admin.html mein `reqList` / `loadRequests` (Approve/Reject); main.c mein `request_post_handler`
- **Whitelist schedule** (HHMM-HHMM): admin.html mein `#newsched`; main.c mein `wl_in_schedule`; time zone setting `tz_offset` (Advanced Settings, default 5)
- **Backup upstream network**: admin.html Upstream WiFi ke andar `#sta_ssid2` / `#sta_pass2`; main.c mein `STA_FAILOVER_FAILS` (kitni baar fail ho to switch, abhi 5) aur `primary_return_task` (primary par wapsi ka check, `tick % 3000` yani ~5 min)
- Naya setting add karna ho to main.c ki `SETTINGS[]` table mein aik line jodo; naya URL add karo to `max_uri_handlers` (abhi 34) check karo
