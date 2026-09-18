// Regression tests for the multi-printer send dialog.
//
// Runs the real method bodies from resources/web/printer/print_send/printsend.js against
// the vendored Vue build, so reactivity and async-ordering bugs are caught without a
// browser. These are the failure shapes that have actually occurred: a stale IPC response
// clobbering the operator's tray picks, and an identity guard comparing a raw object
// against the reactive proxy that is read back.
//
//   node tests/web/test_print_send.js
const fs = require('fs');
const path = require('path').join(__dirname, '..', '..', 'resources', 'web') + '/';
global.self = global; global.window = global; global.document = undefined;
// The UMD bundle prefers CommonJS when module/exports exist; evaluate it in a bare
// context so it takes the browser-global branch, as the webview does.
const vm = require('vm');
const sandbox = { console, setTimeout, clearTimeout, Promise };
sandbox.window = sandbox; sandbox.self = sandbox; sandbox.globalThis = sandbox;
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path + 'thirdparty/vue/vue.global.prod.min.js', 'utf8'), sandbox);
const reactive = sandbox.Vue && sandbox.Vue.reactive;
if (typeof reactive !== 'function') { console.log('FAIL: could not load Vue.reactive'); process.exit(1); }
console.log('Vue loaded:', sandbox.Vue.version);

const src = fs.readFileSync(path + 'printer/print_send/printsend.js', 'utf8');
// Extract a method body by name. Relies on the house 8-space closer: a mis-indented
// method fails to extract and the run exits non-zero rather than testing the wrong thing.
function extract(name, args) {
    const re = new RegExp('async ' + name + '\\(' + args + '\\)\\s*\\{[\\s\\S]*?\\n        \\},\\n');
    const m = src.match(re);
    if (!m) { console.log('FAIL: could not extract ' + name); process.exit(1); }
    return m[0].replace(new RegExp('^async ' + name + '\\(' + args + '\\)\\s*\\{'), '')
               .replace(/\n        \},\n$/, '');
}
// Same, for a plain (non-async) method.
function extractSync(name, args) {
    const re = new RegExp('\\n        ' + name + '\\(' + args + '\\)\\s*\\{[\\s\\S]*?\\n        \\},\\n');
    const m = src.match(re);
    if (!m) { console.log('FAIL: could not extract ' + name); process.exit(1); }
    return m[0].replace(new RegExp('^\\n        ' + name + '\\(' + args + '\\)\\s*\\{'), '')
               .replace(/\n        \},\n$/, '');
}
const body = extract('syncAdditionalPrinterData', '');
const refreshBody = extract('refreshAdditionalPrinter', 'printerId');
const fetchBody = extract('fetchAdditionalPrinterData', 'printerId');

function makeCtx(respond) {
  const ctx = reactive({
    additionalPrinterIds: ['P2'],
    additionalPrinterData: {},
    printerList: [{ printerId: 'P2', printerName: 'CC2-2' }],
  });
  ctx.$t = k => k;
  ctx.ipcRequest = respond;
  ctx.fetchAdditionalPrinterData = fetchFn.bind(ctx);
  ctx.refreshAdditionalPrinter = refreshFn.bind(ctx);
  ctx.sending = false;
  return ctx;
}
const run = new Function('return async function(){' + body + '}')();
const fetchFn = new Function('printerId', 'return (async () => {' + fetchBody + '})()');
const refreshFn = new Function('printerId', 'return (async () => {' + refreshBody + '})()');

// upload() decides which additional printers are sent to, so run the real body
const uploadBody = extract('upload', '');
const runUpload = new Function('return async function(){' + uploadBody + '}')();
const isPrinterBusy = new Function('printer', extractSync('isPrinterBusy', 'printer'));
global.ElLoading = { service: () => ({ close() {} }) };

// A dialog with one additional printer, P2, in whatever state the test needs.
function makeUploadCtx(p2, uploadAndPrint, entry) {
  const captured = { payload: null, blocked: null, tip: null, tips: [], occupied: null };
  const ctx = {
    curPrinter: { printerId: 'P1', printerName: 'CC2-1', connectStatus: 1,
                  systemCapabilities: { supportsMultiFilament: true } },
    printInfo: { selectedPrinterId: 'P1', uploadAndPrint, filamentList: [] },
    printerList: [{ printerId: 'P1', printerName: 'CC2-1', connectStatus: 1, printerStatus: 0 }, p2],
    additionalPrinterIds: ['P2'],
    additionalPrinterData: { P2: entry || { printerName: 'CC2-2', loading: false, hasMms: true, filamentList: [] } },
    selectedBedType: { value: 'btPTE' },
    hasMmsInfo: false,
    mmsInfo: {},
    sending: false,
    slicedFilamentCount: 1,
    captured,
  };
  ctx.$t = (k, a) => k + (a ? '[' + a.join('|') + ']' : '');
  ctx.isPrinterBusy = isPrinterBusy;
  ctx.isPrinterModelNotMatch = () => false;
  ctx.checkAdditionalFilamentMapping = () => true;
  ctx.checkFilamentMapping = () => true;
  ctx.getPrinterStatus = st => 'status' + st;
  ctx.getAdditionalBedType = () => ({ value: 'btPTE' });
  ctx.showStatusTip = t => { captured.tips.push(t); captured.tip = t; };
  ctx.confirmPartialSend = async blocked => { captured.blocked = blocked; return true; };
  ctx.confirmBedsCleared = async printers => { captured.occupied = printers; return true; };
  ctx.ipcRequest = async (_name, data, _timeout, quiet) => {
    captured.payload = data;
    captured.quiet = quiet;
    return {};
  };
  return ctx;
}

(async () => {
  let fails = 0;
  // 1. success path must clear loading and store the response
  let ctx = makeCtx(async () => ({ mmsInfo: {x:1}, hasMms: true, mappedFilamentList: [{index:0}] }));
  await run.call(ctx);
  let e = ctx.additionalPrinterData['P2'];
  const ok1 = e && e.loading === false && e.hasMms === true && e.filamentList.length === 1;
  console.log((ok1?'PASS':'FAIL') + '  success: loading=' + e.loading + ' hasMms=' + e.hasMms + ' filaments=' + e.filamentList.length);
  if (!ok1) fails++;

  // 2. failure path must set readFailed and clear loading
  ctx = makeCtx(async () => { throw new Error('boom'); });
  await run.call(ctx);
  e = ctx.additionalPrinterData['P2'];
  const ok2 = e && e.loading === false && e.readFailed === true;
  console.log((ok2?'PASS':'FAIL') + '  failure: loading=' + e.loading + ' readFailed=' + e.readFailed);
  if (!ok2) fails++;

  // 3. stale response must NOT clobber an entry recreated under the same id.
  //    The replacement is never written to either way, so assert on the ORIGINAL entry:
  //    without the identity guard the in-flight response writes into it.
  ctx = makeCtx(async () => { await new Promise(r => setTimeout(r, 30)); return { hasMms: true, mappedFilamentList: [{index:0, stale:true}] }; });
  const p = run.call(ctx);
  const detached = ctx.additionalPrinterData['P2'];
  delete ctx.additionalPrinterData['P2'];
  ctx.additionalPrinterData['P2'] = { printerName:'CC2-2', loading:false, filamentList:[{index:0, userPick:true}], hasMms:true };
  await p;
  e = ctx.additionalPrinterData['P2'];
  const ok3 = e.filamentList[0].userPick === true
           && detached.loading === true && detached.filamentList.length === 0;
  console.log((ok3?'PASS':'FAIL') + '  stale guard: user pick ' + (e.filamentList[0].userPick ? 'preserved' : 'CLOBBERED') +
              ', discarded response ' + (detached.filamentList.length === 0 ? 'dropped' : 'WRITTEN TO DETACHED ENTRY'));
  if (!ok3) fails++;

  // 4. a refused send re-arms the Send button and says why
  let uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 0 }, true);
  // mirrors the real ipcRequest: it reports the failure itself unless the caller is quiet
  uctx.ipcRequest = async (_n, _d, _t, quiet) => {
    const e = new Error('refused'); e.code = 10004;
    if (!quiet) uctx.showStatusTip(e.message);
    throw e;
  };
  await runUpload.call(uctx);
  const ok4 = uctx.sending === false && uctx.captured.tips.length === 1;
  console.log((ok4?'PASS':'FAIL') + '  refusal re-arms Send, says why once: sending=' +
              uctx.sending + ' tips=' + JSON.stringify(uctx.captured.tips));
  if (!ok4) fails++;

  // 5. a timeout carries no code: the send may still be running, so the button stays
  //    disabled rather than risk enqueueing the plate twice - but it must say so
  uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 0 }, true);
  uctx.ipcRequest = async (_n, _d, _t, quiet) => {
    const e = new Error('timeout');
    if (!quiet) uctx.showStatusTip(e.message);
    throw e;
  };
  await runUpload.call(uctx);
  const ok5 = uctx.sending === true && uctx.captured.tips.length === 1 &&
              uctx.captured.tips[0] === 'printSend.sendTimedOut';
  console.log((ok5?'PASS':'FAIL') + '  timeout holds Send and reports: sending=' + uctx.sending +
              ' tip=' + JSON.stringify(uctx.captured.tip));
  if (!ok5) fails++;

  // 5b. the re-entry guard is set after the validation gates, so a cancelled confirm
  //     does not strand it
  uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 0 }, true,
                       { printerName: 'CC2-2', loading: false, readFailed: true, filamentList: [] });
  uctx.confirmPartialSend = async b => { uctx.captured.blocked = b; return false; };
  await runUpload.call(uctx);
  const ok5b = uctx.sending === false && uctx.captured.payload === null;
  console.log((ok5b?'PASS':'FAIL') + '  cancelled confirm leaves Send armed: sending=' + uctx.sending);
  if (!ok5b) fails++;

  // 6. refresh preserves bedType and the proxy, and re-reads a failed entry clean
  let calls = 0;
  ctx = makeCtx(async () => { calls++; return { hasMms: true, mappedFilamentList: [{index:0}] }; });
  await run.call(ctx);
  e = ctx.additionalPrinterData['P2'];
  e.bedType = 'btPC';
  e.readFailed = true; e.error = 'boom';
  const before = e;
  await ctx.refreshAdditionalPrinter('P2');
  e = ctx.additionalPrinterData['P2'];
  const ok6 = e === before && e.bedType === 'btPC' && e.readFailed === false &&
              e.error === '' && e.loading === false && calls === 2;
  console.log((ok6?'PASS':'FAIL') + '  refresh: bedType=' + e.bedType + ' sameEntry=' + (e === before) +
              ' readFailed=' + e.readFailed + ' ipcCalls=' + calls);
  if (!ok6) fails++;

  // 7. a refresh while already loading issues no IPC
  calls = 0;
  ctx.additionalPrinterData['P2'].loading = true;
  await ctx.refreshAdditionalPrinter('P2');
  const ok7 = calls === 0;
  console.log((ok7?'PASS':'FAIL') + '  refresh while loading: ipcCalls=' + calls);
  if (!ok7) fails++;
  ctx.additionalPrinterData['P2'].loading = false;

  // 8. no refresh while a send is in flight
  calls = 0;
  ctx.sending = true;
  await ctx.refreshAdditionalPrinter('P2');
  const ok8 = calls === 0;
  console.log((ok8?'PASS':'FAIL') + '  refresh while sending: ipcCalls=' + calls);
  if (!ok8) fails++;
  ctx.sending = false;

  // 9. untick during a refresh discards the response. As in test 3, the map entry is
  //    gone either way, so assert the removed entry was left alone too.
  ctx = makeCtx(async () => { await new Promise(r => setTimeout(r, 30)); return { hasMms: true, mappedFilamentList: [{index:0, stale:true}] }; });
  await run.call(ctx);
  ctx.additionalPrinterData['P2'].filamentList = [{index:0, userPick:true}];
  const removed = ctx.additionalPrinterData['P2'];
  const pr = ctx.refreshAdditionalPrinter('P2');
  delete ctx.additionalPrinterData['P2'];
  await pr;
  const ok9 = ctx.additionalPrinterData['P2'] === undefined
           && removed.filamentList.length === 1 && removed.filamentList[0].userPick === true;
  console.log((ok9?'PASS':'FAIL') + '  untick during refresh: entry stays gone=' +
              (ctx.additionalPrinterData['P2'] === undefined) +
              ' removed entry untouched=' + (removed.filamentList[0] && removed.filamentList[0].userPick === true));
  if (!ok9) fails++;

  // 10. a busy additional printer is dropped and named, not sent
  const printing = { printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 1 };
  uctx = makeUploadCtx(printing, true);
  await runUpload.call(uctx);
  let sent = (uctx.captured.payload || {}).additionalPrinters || [];
  const ok10 = sent.length === 0 && uctx.captured.blocked &&
               uctx.captured.blocked.length === 1 &&
               uctx.captured.blocked[0].indexOf('CC2-2') !== -1;
  console.log((ok10?'PASS':'FAIL') + '  busy printer dropped: sent=' + sent.length +
              ' blocked=' + JSON.stringify(uctx.captured.blocked));
  if (!ok10) fails++;

  // 11. and dropped on upload-only too: sendPrintFile refuses the upload itself, so the
  //     check must not be gated on uploadAndPrint. This is the regression that matters.
  uctx = makeUploadCtx(printing, false);
  await runUpload.call(uctx);
  sent = (uctx.captured.payload || {}).additionalPrinters || [];
  const ok11 = sent.length === 0 && uctx.captured.blocked && uctx.captured.blocked.length === 1;
  console.log((ok11?'PASS':'FAIL') + '  busy dropped on upload-only: sent=' + sent.length +
              ' confirmed=' + !!uctx.captured.blocked);
  if (!ok11) fails++;

  // 12. print-completed (16) is NOT busy - isBusy accepts it, so the dialog must too
  uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 16 }, true);
  await runUpload.call(uctx);
  sent = (uctx.captured.payload || {}).additionalPrinters || [];
  const ok12 = sent.length === 1 && sent[0].printerId === 'P2' && uctx.captured.blocked === null;
  console.log((ok12?'PASS':'FAIL') + '  print-completed still sent: sent=' + sent.length +
              ' blocked=' + uctx.captured.blocked);
  if (!ok12) fails++;

  // 13. a disconnected extra is dropped - the non-MMS gap the dialog had no check for
  uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 0, printerStatus: 0 }, true);
  await runUpload.call(uctx);
  sent = (uctx.captured.payload || {}).additionalPrinters || [];
  const ok13 = sent.length === 0 && uctx.captured.blocked && uctx.captured.blocked.length === 1;
  console.log((ok13?'PASS':'FAIL') + '  disconnected printer dropped: sent=' + sent.length);
  if (!ok13) fails++;

  // 13b. a model mismatch is refused whatever the post-action, matching the C++ backstop
  for (const uploadAndPrint of [true, false]) {
    uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 0 }, uploadAndPrint);
    uctx.isPrinterModelNotMatch = p => p && p.printerId === 'P2';
    await runUpload.call(uctx);
    sent = (uctx.captured.payload || {}).additionalPrinters || [];
    const ok = sent.length === 0 && uctx.captured.blocked && uctx.captured.blocked.length === 1;
    console.log((ok?'PASS':'FAIL') + '  model mismatch dropped, uploadAndPrint=' + uploadAndPrint +
                ': sent=' + sent.length);
    if (!ok) fails++;
  }

  // 13c. an MMS-capable printer whose filament system is offline cannot run a
  //      multi-filament print; the backstop refuses it, so the dialog must say so
  uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 0,
                         systemCapabilities: { supportsMultiFilament: true } }, true,
                       { printerName: 'CC2-2', loading: false, hasMms: false, filamentList: [] });
  uctx.slicedFilamentCount = 2;
  await runUpload.call(uctx);
  sent = (uctx.captured.payload || {}).additionalPrinters || [];
  const ok13c = sent.length === 0 && uctx.captured.blocked && uctx.captured.blocked.length === 1;
  console.log((ok13c?'PASS':'FAIL') + '  mms-offline blocked for multi-filament: sent=' + sent.length +
              ' blocked=' + JSON.stringify(uctx.captured.blocked));
  if (!ok13c) fails++;

  // 13d. the same printer is fine for a single-filament plate
  uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 0,
                         systemCapabilities: { supportsMultiFilament: true } }, true,
                       { printerName: 'CC2-2', loading: false, hasMms: false, filamentList: [] });
  uctx.slicedFilamentCount = 1;
  await runUpload.call(uctx);
  sent = (uctx.captured.payload || {}).additionalPrinters || [];
  const ok13d = sent.length === 1 && uctx.captured.blocked === null;
  console.log((ok13d?'PASS':'FAIL') + '  mms-offline allowed for single filament: sent=' + sent.length);
  if (!ok13d) fails++;

  // 13e. an offline printer cannot be chosen, but one that goes offline after being
  //      chosen must still be removable, or the operator is stuck with it
  const toggle = new Function('printer', extractSync('toggleAdditionalPrinter', 'printer'));
  const tctx = { additionalPrinterIds: [], syncAdditionalPrinterData() {}, resizeWindow() {},
                 isPrinterModelNotMatch: () => false };
  toggle.call(tctx, { printerId: 'P2', connectStatus: 0 });
  const offlineNotAdded = tctx.additionalPrinterIds.length === 0;
  toggle.call(tctx, { printerId: 'P2', connectStatus: 1 });
  const onlineAdded = tctx.additionalPrinterIds.length === 1;
  toggle.call(tctx, { printerId: 'P2', connectStatus: 0 });   // it went offline meanwhile
  const offlineRemovable = tctx.additionalPrinterIds.length === 0;
  const ok13e = offlineNotAdded && onlineAdded && offlineRemovable;
  console.log((ok13e?'PASS':'FAIL') + '  offline: not choosable=' + offlineNotAdded +
              ' chosen=' + onlineAdded + ' still removable=' + offlineRemovable);
  if (!ok13e) fails++;

  // 13f. standardizeFilamentType: the fold the tray comparison depends on. The slicer
  //      under-reports a reinforcement suffix (a PETG-CF preset reports PETG) while the
  //      printer reports the tray as PETG-CF, so both sides are normalised before
  //      comparing. "-S" is NOT folded: get_filament_type() adds it to mark support
  //      material, and folding it would match a support slice to a model-material tray.
  const stdType = new Function('type', extractSync('standardizeFilamentType', 'type'));
  const typeCases = [
    ['PETG',        'PETG',  'plain type unchanged'],
    ['PETG-CF',     'PETG',  'reinforcement folded'],
    ['PETG-GF',     'PETG',  'glass fibre folded'],
    ['PLA-AERO',    'PLA',   'aero folded'],
    ['PETG-CF10',   'PETG',  'graded fibre folded'],
    ['UltraPA-CF25','ULTRAPA','graded fibre on a compound base'],
    ['TPU-95A',     'TPU',   'durometer folded'],
    ['TPU-64D',     'TPU',   'durometer folded'],
    ['PLA+',        'PLA',   'trailing plus dropped'],
    ['PLA-S',       'PLA-S', 'support NOT folded'],
    ['PA-S',        'PA-S',  'support NOT folded'],
    ['PAHT-CF',     'PAHT',  'PAHT is not PA'],
    ['PA6-CF',      'PA6',   'PA6 is not PA'],
    ['PETG-ESD',    'PETG-ESD','unknown suffix kept'],
    ['',            '',      'empty'],
    ['petg-cf',     'PETG',  'case insensitive'],
  ];
  const badTypes = typeCases.filter(c => stdType(c[0]) !== c[1]);
  const ok13f = badTypes.length === 0;
  console.log((ok13f?'PASS':'FAIL') + '  standardizeFilamentType: ' + typeCases.length + ' cases' +
              (ok13f ? '' : ' - wrong for ' + badTypes.map(c => c[0]+'->'+stdType(c[0])+' (want '+c[1]+')').join(', ')));
  if (!ok13f) fails++;

  // 13g. a wrong-model printer cannot be ticked at all - it can never run this gcode,
  //      so discovering that at Send would waste the operator's mapping work
  const toggle2 = new Function('printer', extractSync('toggleAdditionalPrinter', 'printer'));
  const mctx = {
    additionalPrinterIds: [], syncAdditionalPrinterData() {}, resizeWindow() {},
    isPrinterModelNotMatch: p => p && p.printerModel === 'Elegoo Neptune 4',
  };
  toggle2.call(mctx, { printerId: 'P2', connectStatus: 1, printerModel: 'Elegoo Neptune 4' });
  const wrongModelBlocked = mctx.additionalPrinterIds.length === 0;
  toggle2.call(mctx, { printerId: 'P3', connectStatus: 1, printerModel: 'Elegoo Centauri Carbon 2' });
  const rightModelAdded = mctx.additionalPrinterIds.length === 1;
  // one already chosen that later reads as wrong-model must still be removable
  mctx.additionalPrinterIds.push('P4');
  toggle2.call(mctx, { printerId: 'P4', connectStatus: 1, printerModel: 'Elegoo Neptune 4' });
  const stillRemovable = !mctx.additionalPrinterIds.includes('P4');
  const ok13g = wrongModelBlocked && rightModelAdded && stillRemovable;
  console.log((ok13g?'PASS':'FAIL') + '  wrong model: not choosable=' + wrongModelBlocked +
              ' same model chosen=' + rightModelAdded + ' still removable=' + stillRemovable);
  if (!ok13g) fails++;

  // 13h. isPrinterModelNotMatch itself. 13g stubs it, so these cases are invisible there.
  //      The rule must match resolveAdditionalPrinter: a mismatch only when BOTH sides
  //      report a model. A dialog stricter than the backstop is a dead end with no override.
  const modelNotMatch = new Function('printer', extractSync('isPrinterModelNotMatch', 'printer'));
  const mmCtx = { printInfo: { currentProjectPrinterModel: 'Elegoo Centauri Carbon 2' } };
  const call = (p, ctx) => modelNotMatch.call(ctx || mmCtx, p);
  const modelCases = [
    [{ printerModel: 'Elegoo Centauri Carbon 2' }, false, 'same model'],
    [{ printerModel: 'Elegoo Neptune 4' },         true,  'different model'],
    [{ printerModel: '' },                         false, 'printer model empty -> not a proven mismatch'],
    [{},                                           false, 'printer model absent -> not a proven mismatch'],
    [undefined,                                    false, 'printer missing from the list'],
  ];
  const badModels = modelCases.filter(c => call(c[0]) !== c[1]);
  // project side empty: nothing to compare against
  const projectEmpty = call({ printerModel: 'Elegoo Neptune 4' }, { printInfo: { currentProjectPrinterModel: '' } });
  const noPrintInfo  = call({ printerModel: 'Elegoo Neptune 4' }, { printInfo: null });
  const ok13h = badModels.length === 0 && projectEmpty === false && noPrintInfo === false;
  console.log((ok13h?'PASS':'FAIL') + '  isPrinterModelNotMatch: ' + modelCases.length + ' cases + 2 project-side' +
              (ok13h ? '' : ' - wrong for ' + badModels.map(c => c[2]).join(', ') +
               ' projectEmpty=' + projectEmpty + ' noPrintInfo=' + noPrintInfo));
  if (!ok13h) fails++;

  // 13i. a printer that has FINISHED a print still has the part on the bed. The primary
  //      warns; an extra must too, or the nozzle homes into yesterday's part. Status 16
  //      is styled the same green as idle, so there is no other cue.
  uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 16 }, true);
  await runUpload.call(uctx);
  sent = (uctx.captured.payload || {}).additionalPrinters || [];
  const ok13i = uctx.captured.occupied && uctx.captured.occupied.length === 1 && sent.length === 1;
  console.log((ok13i?'PASS':'FAIL') + '  finished printer prompts before sending: asked=' +
              !!uctx.captured.occupied + ' sent=' + sent.length);
  if (!ok13i) fails++;

  // 13j. declining that prompt sends nothing at all
  uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 16 }, true);
  uctx.confirmBedsCleared = async () => false;
  await runUpload.call(uctx);
  const ok13j = uctx.captured.payload === null;
  console.log((ok13j?'PASS':'FAIL') + '  declining the bed prompt cancels the send: payload=' +
              (uctx.captured.payload === null ? 'none' : 'SENT'));
  if (!ok13j) fails++;

  // 13k. an idle printer is never asked
  uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 0 }, true);
  await runUpload.call(uctx);
  const ok13k = uctx.captured.occupied === null;
  console.log((ok13k?'PASS':'FAIL') + '  idle printer not prompted: asked=' + !!uctx.captured.occupied);
  if (!ok13k) fails++;

  // 13l. the bed prompt is asked LAST, about printers actually being sent to. A printer
  //      that another gate drops must not be asked about: confirming a bed and then being
  //      told that printer cannot receive the job is the double-prompt the ordering fixes.
  uctx = makeUploadCtx({ printerId: 'P2', printerName: 'CC2-2', connectStatus: 1, printerStatus: 16 }, true);
  uctx.checkAdditionalFilamentMapping = () => false;          // dropped for unmapped filament
  await runUpload.call(uctx);
  const ok13l = uctx.captured.occupied === null && uctx.captured.blocked
             && uctx.captured.blocked.length === 1;
  console.log((ok13l?'PASS':'FAIL') + '  dropped printer not asked about its bed: asked=' +
              !!uctx.captured.occupied + ' blocked=' + (uctx.captured.blocked || []).length);
  if (!ok13l) fails++;

  // 14. isPrinterBusy across every state, including the ones ElegooLink::isBusy
  //     treats differently (a failed status query is not busy there, busy here)
  const busyCases = [
    [{ connectStatus: 1, printerStatus: 0 }, false, 'idle'],
    [{ connectStatus: 1, printerStatus: 16 }, false, 'print completed'],
    [{ connectStatus: 1, printerStatus: 1 }, true, 'printing'],
    [{ connectStatus: 1, printerStatus: 2 }, true, 'paused'],
    [{ connectStatus: 1, printerStatus: 1001 }, true, 'auth error'],
    [{ connectStatus: 1, printerStatus: 10000 }, true, 'unknown'],
    [{ connectStatus: 0, printerStatus: 0 }, true, 'disconnected but idle'],
    [undefined, false, 'printer not in list'],
  ];
  const bad = busyCases.filter(c => isPrinterBusy(c[0]) !== c[1]);
  const ok14 = bad.length === 0;
  console.log((ok14?'PASS':'FAIL') + '  isPrinterBusy states' +
              (ok14 ? '' : ': wrong for ' + bad.map(c => c[2]).join(', ')));
  if (!ok14) fails++;

  console.log(fails ? '\n' + fails + ' FAILURE(S)' : '\nall pass');
  process.exit(fails ? 1 : 0);
})();
