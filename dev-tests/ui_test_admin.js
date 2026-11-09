// Developer test: loads main/admin.html in a real headless Chromium at phone size (380px) with a mock backend
// built from the real SETTINGS[] table in main.c, then clicks every section/button and checks the results.
// Run:  NODE_PATH=<global node_modules> node dev-tests/ui_test_admin.js   (needs the "playwright" package)
const { chromium } = require('playwright');
const fs = require('fs');
const dir = require('path').join(__dirname, '..', 'main') + '/';
const html = fs.readFileSync(dir + 'admin.html', 'utf8');
const csrc = fs.readFileSync(dir + 'main.c', 'utf8');

// Build /api/settings from the REAL SETTINGS[] table in main.c
const tbl = csrc.match(/static const setting_t SETTINGS\[\] = \{([\s\S]*?)\n\};/)[1];
const items = [];
for (const line of tbl.split('\n')) {
  const m = line.match(/^\s*\{"([a-z_0-9]+)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)",\s*"([a-z_]+)",\s*"((?:[^"\\]|\\.)*)",\s*"([a-z]+)"\}/);
  if (m) items.push({ key: m[1], label: m[2], def: m[3], value: m[4] === 'password_keep' ? '' : m[3], type: m[4], hint: m[5], group: m[6] });
}
console.log('settings parsed from main.c:', items.length);

const status = {
  fw: '2.1.0', sta_connected: true, sta_ip: '192.168.1.50', uptime_s: 90000, free_heap: 150000,
  sta_ssid: 'HomeNet', ap_ip: '192.168.4.1', wl_active: true, session_uptime_s: 4000, discon_total: 2,
  clients: [{ mac: 'AA:BB:CC:DD:EE:01', ip: '192.168.4.2', name: 'Ali phone', rssi: -55, authorized: true },
            { mac: 'AA:BB:CC:DD:EE:02', ip: '192.168.4.3', name: '', rssi: -70, authorized: false }],
  whitelist: [{ mac: 'AA:BB:CC:DD:EE:01', name: 'Ali phone', limit_kbps: 500 }],
  discon_log: [{ ago_s: 300, reason: 201 }, { ago_s: 9000, reason: 3 }]
};
const usage = { time_synced: true, devices: [{ mac: 'AA:BB:CC:DD:EE:01', name: 'Ali phone', total_bytes: 5e8, week_bytes: 2e7, month_bytes: 9e7 }],
  overall: { upload_total_bytes: 5e8, upload_week_bytes: 2e7, upload_month_bytes: 9e7, download_total_bytes: 5e9, download_week_bytes: 3e8, download_month_bytes: 1e9 } };
const speed = { samples: [{ ago_s: 10, ms: 30, ok: true }, { ago_s: 900, ms: 80, ok: true }, { ago_s: 1800, ms: 0, ok: false }] };

(async () => {
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 380, height: 800 }, deviceScaleFactor: 2, isMobile: true });
  const page = await ctx.newPage();
  const errors = [], posts = [];
  page.on('pageerror', e => errors.push('PAGEERROR: ' + e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push('CONSOLE: ' + m.text()); });
  page.on('dialog', d => d.accept());

  await page.route('http://repeater.test/**', async route => {
    const u = new URL(route.request().url()); const p = u.pathname; const method = route.request().method();
    const json = o => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(o) });
    if (p === '/admin') return route.fulfill({ status: 200, contentType: 'text/html', body: html });
    if (method === 'POST') { posts.push({ path: p, body: route.request().postData() }); return json({ ok: true }); }
    if (p === '/api/status') return json(status);
    if (p === '/api/settings') return json({ items });
    if (p === '/api/usage') return json(usage);
    if (p === '/api/speedhistory') return json(speed);
    if (p === '/api/config/export') return route.fulfill({ status: 200, contentType: 'application/json', body: '{"ap_ssid":"Sarfraz"}' });
    return route.fulfill({ status: 404, body: 'nf' });
  });

  await page.goto('http://repeater.test/admin');
  await page.waitForTimeout(800);

  const result = {};
  const T = (name, ok, extra) => { result[name] = ok; console.log((ok ? 'PASS ' : 'FAIL ') + name + (extra ? '  -> ' + extra : '')); };

  // 1. Status grid rendered (not stuck on Loading...)
  await page.evaluate(() => window.go('p-info'));
  await page.waitForTimeout(150);
  const statTxt = await page.locator('#statGrid').innerText();
  T('status grid renders real data', /Connected/.test(statTxt) && /192\.168\.1\.50/.test(statTxt), statTxt.replace(/\n/g, ' | ').slice(0, 90));
  T('header shows firmware version', /v2\.1\.0/.test(await page.locator('#fwLine').innerText()));
  T('arrow icon is real symbol, not "\\25BE" text', !(await page.evaluate(() => getComputedStyle(document.querySelector('summary'), '::after').content)).includes('25BE'),
    await page.evaluate(() => getComputedStyle(document.querySelector('summary'), '::after').content));

  // 2. Every section shows through the menu (one section at a time)
  const goSec = id => page.evaluate(i => window.go(i), id);
  for (const id of ['p-home', 'p-info', 'd-usage', 'd-speed', 'd-stab', 'd-wifi', 'd-wl', 'd-adv', 'd-backup', 'd-ota', 'd-danger']) {
    await goSec(id);
    await page.waitForTimeout(150);
    T('section shows: ' + id, await page.locator('#' + id).evaluate(e => e.classList.contains('show')));
    if (id.startsWith('d-')) T('details open: ' + id, await page.locator('#' + id).evaluate(e => e.open));
  }
  T('only one section visible at a time', (await page.locator('.sec.show').count()) === 1);
  await page.locator('#nv-home').click(); await page.waitForTimeout(150);
  T('menu: Home Page click shows p-home', await page.locator('#p-home').evaluate(e => e.classList.contains('show')));
  await page.locator('g[onclick="toggleDevs()"]').click(); await page.waitForTimeout(200);
  T('topology click shows d-clients on Home', await page.locator('#d-clients').evaluate(e => e.classList.contains('show')));
  await page.waitForTimeout(300);

  // 3. Client + whitelist tables populated
  T('connected devices shown as 2 cards', (await page.locator('#clientsTbl .dev').count()) === 2);
  T('device card shows MAC, IP and signal', /AA:BB:CC:DD:EE:01 · 192\.168\.4\.2 · -55 dBm/.test(await page.locator('#clientsTbl').innerText()));
  T('whitelist shows entry with limit', /500 Kbps/.test(await page.locator('#wlList').innerText()));
  T('stability log shows plain-English reason', /Router not found/.test(await page.locator('#stabBody').innerText()));
  T('usage totals rendered', /MB|GB/.test(await page.locator('#overallUsage').innerText()));
  T('speed history bars drawn', (await page.locator('#shChart i').count()) === 3);

  await goSec('d-wifi');
  // 4. Upstream WiFi: show-password toggle + Save & Reboot
  await page.fill('#sta_ssid', 'MyRouter');
  await page.fill('#sta_pass', 'secret1234');
  await page.locator('#d-wifi button:has-text("SHOW")').click();
  T('SHOW reveals upstream password', (await page.locator('#sta_pass').getAttribute('type')) === 'text');
  await page.locator('#d-wifi button:has-text("HIDE")').click();
  T('HIDE re-masks password', (await page.locator('#sta_pass').getAttribute('type')) === 'password');
  await page.locator('#d-wifi button:has-text("Save")').click();
  await page.waitForTimeout(300);
  const wp = posts.find(p => p.path === '/api/wifi');
  T('Save & Reboot sends POST /api/wifi', !!wp && JSON.parse(wp.body).ssid === 'MyRouter' && JSON.parse(wp.body).password === 'secret1234', wp && wp.body);
  T('Save shows feedback message', /rebooting/i.test(await page.locator('#wifiMsg').innerText()));

  await goSec('d-adv');
  // 5. Advanced settings: grouped, fields, show/hide, fill defaults, save
  const groups = await page.locator('#advGroups > details.subgroup > summary').allInnerTexts();
  T('advanced settings has 4 groups', groups.length === 4, groups.join(' | '));
  T('every setting rendered as a field', (await page.locator('#advGroups [data-key]').count()) === items.length, (await page.locator('#advGroups [data-key]').count()) + '/' + items.length);
  await page.locator('#advGroups > details.subgroup > summary').first().click();
  const apPassBtn = page.locator('#advGroups button:has-text("SHOW")').first();
  await apPassBtn.click();
  T('SHOW works inside advanced settings', (await page.locator('#fld_ap_pass').getAttribute('type')) === 'text');
  await page.locator('#d-adv button:has-text("Fill all defaults")').click();
  T('fill defaults sets ap_ssid=Sarfraz', (await page.locator('[data-key="ap_ssid"]').inputValue()) === 'Sarfraz');
  await page.locator('#d-adv button:has-text("Save")').click();
  await page.waitForTimeout(300);
  const sp = posts.find(p => p.path === '/api/settings');
  T('Save settings sends every key', !!sp && Object.keys(JSON.parse(sp.body)).length === items.length, sp && Object.keys(JSON.parse(sp.body)).length + ' keys');

  await goSec('d-wl');
  // 6. Whitelist add / remove, kick
  await page.fill('#newmac', 'AA:BB:CC:00:11:22'); await page.fill('#newname', 'Test'); await page.fill('#newlimit', '300');
  await page.locator('#d-wl button:has-text("Add to Whitelist")').click(); await page.waitForTimeout(300);
  const wl = posts.find(p => p.path === '/api/whitelist' && JSON.parse(p.body).action === 'add');
  T('whitelist add POST has mac/name/limit', !!wl && JSON.parse(wl.body).limit_kbps === 300);
  await page.locator('#wlList button:has-text("Remove")').first().click(); await page.waitForTimeout(300);
  T('whitelist remove POST sent', posts.some(p => p.path === '/api/whitelist' && JSON.parse(p.body).action === 'remove'));
  await goSec('p-home'); await page.evaluate(() => { const d = document.getElementById('d-clients'); if (!d.classList.contains('show')) toggleDevs(); });
  await page.locator('#clientsTbl button:has-text("Kick")').first().click(); await page.waitForTimeout(300);
  T('kick POST sent', posts.some(p => p.path === '/api/kick'));
  await page.locator('#clientsTbl button:has-text("+WL")').nth(1).click();
  T('+WL prefills MAC field', (await page.inputValue('#newmac')) === 'AA:BB:CC:DD:EE:02');

  await goSec('d-backup');
  // 7. Backup + danger zone + logout
  await page.locator('#d-backup button:has-text("Export")').click(); await page.waitForTimeout(300);
  T('export fills the box', /Sarfraz/.test(await page.inputValue('#importbox')));
  await page.locator('#d-backup button:has-text("Import")').click(); await page.waitForTimeout(300);
  T('import POST sent', posts.some(p => p.path === '/api/config/import'));
  await goSec('d-danger');
  await page.locator('#d-danger button:has-text("Reboot")').click(); await page.waitForTimeout(200);
  T('reboot POST sent', posts.some(p => p.path === '/api/reboot'));
  await page.locator('#d-danger button:has-text("Factory Reset")').click(); await page.waitForTimeout(200);
  T('factory reset POST sent', posts.some(p => p.path === '/api/factory_reset'));

  // 8. Firmware Update section (no real upload is ever sent)
  await goSec('d-ota');
  T('ota: file input and upload button exist', (await page.locator('#otafile').count()) === 1 && (await page.locator('#d-ota button:has-text("Upload")').count()) === 1);
  await page.locator('#d-ota button:has-text("Upload")').click(); await page.waitForTimeout(200);
  T('ota: no file chosen -> message, no request', /choose/i.test(await page.locator('#otaStatus').innerText()) && !posts.some(p => p.path === '/api/ota'), await page.locator('#otaStatus').innerText());
  await page.setInputFiles('#otafile', { name: 'tiny.bin', mimeType: 'application/octet-stream', buffer: Buffer.alloc(10) });
  await page.locator('#d-ota button:has-text("Upload")').click(); await page.waitForTimeout(200);
  T('ota: tiny file rejected in browser, no request', /size/i.test(await page.locator('#otaStatus').innerText()) && !posts.some(p => p.path === '/api/ota'), await page.locator('#otaStatus').innerText());

  T('no JS errors on the page', errors.length === 0, errors.join(' || '));

  // Screenshots for visual check: one per menu section
  for (const id of ['p-home', 'p-info', 'd-usage', 'd-speed', 'd-stab', 'd-wifi', 'd-wl', 'd-adv', 'd-backup', 'd-ota', 'd-danger']) {
    await goSec(id);
    if (id === 'p-home') await page.evaluate(() => { const d = document.getElementById('d-clients'); if (!d.classList.contains('show')) toggleDevs(); });
    if (id === 'd-adv') await page.evaluate(() => document.querySelectorAll('#advGroups details').forEach(d => d.open = true));
    await page.waitForTimeout(300);
    await page.screenshot({ path: '/tmp/admin_' + id + '.png', fullPage: true });
  }
  // layout guard: nothing may be wider than the 380px screen
  const overflow = await page.evaluate(() => { const w = document.documentElement.clientWidth; const bad = []; document.querySelectorAll('button,input,select,textarea,.card,.stat').forEach(e => { const r = e.getBoundingClientRect(); if (r.width > 0 && (r.right > w + 1 || r.left < -1)) bad.push((e.id || e.className || e.tagName) + ':' + Math.round(r.right)); }); return {w, bad}; });
  T('no element overflows the screen width', overflow.bad.length === 0, 'screen ' + overflow.w + 'px ' + overflow.bad.slice(0,6).join(', '));

  const fails = Object.entries(result).filter(([k, v]) => !v).map(([k]) => k);
  console.log('\nTOTAL', Object.keys(result).length, 'checks, FAILED:', fails.length, fails);
  await browser.close();
})();
