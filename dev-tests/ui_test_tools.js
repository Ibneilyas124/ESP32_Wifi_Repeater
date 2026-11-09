// Developer test: loads main/tools.html in headless Chromium at the design width (1000px) with a mock backend.
// Run:  NODE_PATH=<global node_modules> node dev-tests/ui_test_tools.js   (needs the "playwright" package)
// NOTE: mock JSON shapes below are guesses; if only "no JS errors" fails, check the mock first.
const { chromium } = require('playwright');
const fs = require('fs');
const html = fs.readFileSync(require('path').join(__dirname, '..', 'main') + '/tools.html', 'utf8');

(async () => {
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 1000, height: 800 } });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push('PAGEERROR: ' + e.message));
  await page.route('http://repeater.test/**', async route => {
    const p = new URL(route.request().url()).pathname;
    const json = o => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(o) });
    if (p === '/tools') return route.fulfill({ status: 200, contentType: 'text/html', body: html });
    if (p === '/api/me') return json({ admin: false, fw: '2.1.0' });
    if (p === '/api/speedhistory') return json({ samples: [{ ago_s: 10, ms: 30, ok: true }, { ago_s: 900, ms: 0, ok: false }] });
    if (p === '/api/deauth') return json({ enabled: true, level: 'ok', count10: 0, targets_us: false, events: [] });
    if (p === '/api/diag/start') return json({ ok: true });
    if (p === '/api/diag') return json({ state: 'done', connected: true, ssid: 'Test', channel: 6, rssi: -60, ip: '192.168.1.5', gw: 'ok', gw_ip: '192.168.1.1', gw_ms: 5, inet: [{ host: '1.1.1.1', ok: true, ms: 20 }], dns_ok: true, dns_ip: '1.2.3.4', dns_ms: 10, client_dns: '8.8.8.8' });
    return route.fulfill({ status: 404, body: 'nf' });
  });
  await page.goto('http://repeater.test/tools');
  await page.waitForTimeout(800);

  const result = {};
  const T = (name, ok, extra) => { result[name] = ok; console.log((ok ? 'PASS ' : 'FAIL ') + name + (extra ? '  -> ' + extra : '')); };

  const need = ['tb-an','tb-de','tb-sp','tb-dg','p-an','p-de','p-sp','p-dg','scanBtn','spBtn','dgBtn','scanOut','netBanner','dePill','histChart'];
  for (const id of need) T('element exists: ' + id, (await page.locator('#' + id).count()) === 1);
  T('36 ids present', (await page.evaluate(() => document.querySelectorAll('[id]').length)) === 36, String(await page.evaluate(() => document.querySelectorAll('[id]').length)));

  for (const t of ['an', 'de', 'sp', 'dg']) {
    await page.locator('#tb-' + t).click(); await page.waitForTimeout(250);
    const vis = {};
    for (const x of ['an', 'de', 'sp', 'dg']) vis[x] = await page.locator('#p-' + x).isVisible();
    T('tab ' + t + ': only its panel visible', Object.entries(vis).every(([k, v]) => v === (k === t)), JSON.stringify(vis));
    T('tab ' + t + ': button marked on', (await page.locator('#tb-' + t).getAttribute('class')) === 'on');
    await page.screenshot({ path: '/tmp/tools_' + t + '.png', fullPage: true });
  }
  const w = await page.evaluate(() => ({ sw: document.documentElement.scrollWidth, cw: document.documentElement.clientWidth }));
  T('no horizontal overflow at 1000px', w.sw <= w.cw, JSON.stringify(w));
  T('no JS errors (mock shape guessed)', errors.length === 0, errors.join(' || '));

  // /tools#<tab>: admin ka One-Click Diagnosis link /tools#dg kholta hai; har hash par sahi tab khulna chahiye
  const errBefore = errors.length;
  for (const h of ['dg', 'sp', 'de', 'an', 'zzz']) {
    const want = (h === 'zzz') ? 'an' : h;
    await page.goto('about:blank');
    await page.goto('http://repeater.test/tools#' + h);
    await page.waitForTimeout(h === 'dg' ? 2500 : 700);
    const vis = {};
    for (const x of ['an', 'de', 'sp', 'dg']) vis[x] = await page.locator('#p-' + x).isVisible();
    T('hash #' + h + ': only ' + want + ' panel visible', Object.entries(vis).every(([k, v]) => v === (k === want)), JSON.stringify(vis));
    T('hash #' + h + ': tb-' + want + ' marked on', (await page.locator('#tb-' + want).getAttribute('class')) === 'on');
    if (h === 'dg') {
      T('hash #dg: diagnosis ran by itself', (await page.locator('#dgOut').innerText()).includes('Upstream: Test'), await page.locator('#dgOut').innerText());
      T('hash #dg: Run button enabled again', await page.locator('#dgBtn').isEnabled());
    }
  }
  T('no JS errors after hash loads', errors.length === errBefore, errors.slice(errBefore).join(' || '));

  const fails = Object.entries(result).filter(([k, v]) => !v).map(([k]) => k);
  console.log('\nTOTAL', Object.keys(result).length, 'checks, FAILED:', fails.length, fails);
  await browser.close();
})();
