/* Cookieless portal extension. Uses the gateway's closure-authenticated fetch. */
(function (root) {
  'use strict';
  function buildUI(window, document) {
    var section, status, from, to, apply, todayButton, refresh, previous, next, pages, cards;
    var active = false, generation = 0, busy = false, page = 1, totalPages = 0, listOK = false;
    var observer;
    var UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
    function granted() {
      return document.documentElement.getAttribute('data-hg-auth') === 'viewer' &&
        document.documentElement.getAttribute('data-hg-household') === 'granted';
    }
    function element(tag, text, id, parent) {
      var node = document.createElement(tag);
      if (text !== undefined) node.textContent = text;
      if (id) node.id = 'hg-ev-' + id;
      if (parent) parent.appendChild(node);
      return node;
    }
    function today() {
      var parts = new window.Intl.DateTimeFormat('en-CA', { timeZone: 'Asia/Kolkata', year: 'numeric', month: '2-digit', day: '2-digit' }).formatToParts(new window.Date());
      function part(type) { return parts.filter(function (p) { return p.type === type; })[0].value; }
      return part('year') + '-' + part('month') + '-' + part('day');
    }
    function controls() {
      [from, to, apply, todayButton, refresh].forEach(function (n) { n.disabled = !active || busy; });
      previous.disabled = !active || busy || !listOK || page <= 1;
      next.disabled = !active || busy || !listOK || page >= totalPages;
      viewButtons.forEach(function (n) { n.disabled = !active || busy; });
      if (playButton) playButton.disabled = !active || busy;
    }
    var detail, detailStatus, video, playButton, selected = null, viewButtons = [];
    function unload() {
      if (video) { video.pause(); video.removeAttribute('src'); video.load(); }
    }
    function clearDetail() {
      unload(); selected = null; video = null; playButton = null; detailStatus = null;
      if (detail) detail.textContent = '';
    }
    function invalidate() { generation += 1; busy = false; unload(); return generation; }
    function current(g) { return g === generation && active && granted(); }
    function dateValid(s) {
      if (!/^\d{4}-\d{2}-\d{2}$/.test(s)) return false;
      var d = new window.Date(s + 'T00:00:00Z');
      return Number.isFinite(d.getTime()) && d.toISOString().slice(0, 10) === s;
    }
    function record(e) {
      return e && typeof e === 'object' && UUID.test(e.id) && e.provenance === 'real' &&
        typeof e.detected_at === 'string' && Number.isFinite(new window.Date(e.detected_at).getTime()) &&
        ['event_id', 'alert_id', 'summary', 'severity', 'detector'].every(function (k) { return typeof e[k] === 'string'; }) &&
        ['confidence', 'duration_seconds', 'size_bytes'].every(function (k) { return typeof e[k] === 'number' && Number.isFinite(e[k]) && e[k] >= 0; }) && e.confidence <= 1;
    }
    function fail(body) {
      var err = new Error('unavailable');
      if (body && (body.configured === false || /unconfigured|not_configured/.test(String(body.error || '')))) err.unconfigured = true;
      return err;
    }
    async function request(path, g) {
      var response = await window.fetch(path, { method: 'GET', credentials: 'omit', cache: 'no-store', referrerPolicy: 'no-referrer' });
      if (!current(g)) return null;
      var body = await response.json();
      if (!current(g)) return null;
      if (!response.ok || !body || body.ok !== true) {
        var error = fail(body); error.status = response.status; throw error;
      }
      return body;
    }
    function unavailable(error, node) {
      if (!granted()) { sync(); return; }
      if (error.status === 401 || error.status === 403) {
        // Module-local fail closed; never mutate the gateway's auth attributes.
        active = false; invalidate(); clearDetail(); cards.textContent = ''; viewButtons = [];
        listOK = false; status.textContent = ''; pages.textContent = ''; section.hidden = true; controls(); return;
      }
      node.textContent = error.unconfigured ? 'Event videos are not configured. No recordings are being reported.' :
        'Event videos unavailable. This is not an empty event history. Try Refresh or sign in again.';
    }
    function timestamp(s) {
      return new window.Intl.DateTimeFormat('en-IN', { timeZone: 'Asia/Kolkata', dateStyle: 'medium', timeStyle: 'medium' }).format(new window.Date(s)) + ' IST';
    }
    function summary(e, parent) {
      element('p', timestamp(e.detected_at), null, parent);
      element('p', e.summary, null, parent);
      element('p', 'Severity: ' + e.severity + ' · Detector: ' + e.detector, null, parent);
    }
    async function loadList(deepID) {
      if (!active || !granted()) { sync(); return; }
      var g = invalidate(); clearDetail(); cards.textContent = ''; viewButtons = []; listOK = false; pages.textContent = '';
      if (!dateValid(from.value) || !dateValid(to.value) || from.value > to.value) {
        status.textContent = 'Choose valid inclusive From and To dates (From must not follow To).'; controls(); return;
      }
      busy = true; controls(); status.textContent = 'Loading real detection clips…';
      var requestedPage = page, requestedFrom = from.value, requestedTo = to.value;
      try {
        var body = await request('/api/event-videos?from=' + requestedFrom + '&to=' + requestedTo + '&page=' + requestedPage + '&page_size=12', g);
        if (!current(g)) return;
        if (!body || body.configured === false) throw fail(body);
        if (!Array.isArray(body.events) || body.events.length > 12 || body.page !== requestedPage || body.page_size !== 12 ||
            !Number.isSafeInteger(body.total) || body.total < 0 || !Number.isSafeInteger(body.total_pages) ||
            body.total_pages !== Math.ceil(body.total / 12) || body.events.length > body.total ||
            body.from !== requestedFrom || body.to !== requestedTo || body.time_zone !== 'Asia/Kolkata' ||
            !body.events.every(record) || new Set(body.events.map(function (e) { return e.id; })).size !== body.events.length ||
            (body.total > 0 && requestedPage <= body.total_pages && body.events.length !== Math.min(12, body.total - (requestedPage - 1) * 12))) throw fail();
        totalPages = body.total_pages; listOK = true;
        pages.textContent = 'Page ' + page + ' of ' + totalPages + ' · ' + body.total + ' events';
        status.textContent = body.events.length ? 'Real detection-triggered clips · dates inclusive, Asia/Kolkata.' : 'No real detection clips in this date range.';
        body.events.forEach(function (e) {
          var card = element('article', undefined, null, cards);
          summary(e, card);
          var button = element('button', 'View event', null, card); button.type = 'button';
          button.addEventListener('click', function () { select(e.id); }); viewButtons.push(button);
        });
      } catch (error) { if (current(g)) unavailable(error, status); }
      finally { if (current(g)) { busy = false; controls(); } }
      if (deepID && current(g)) { await select(deepID); }
    }
    async function select(id) {
      if (!active || !granted() || !UUID.test(id)) return;
      var g = invalidate(); clearDetail(); busy = true; controls();
      detailStatus = element('p', 'Loading selected event…', 'detail-status', detail);
      try {
        var body = await request('/api/event-videos/' + id, g);
        if (!current(g)) return;
        if (!body || !record(body.event) || body.event.id !== id) throw fail();
        var e = body.event; selected = id;
        detail.textContent = '';
        element('h3', 'Selected event', null, detail);
        summary(e, detail);
        var localDate = new window.Intl.DateTimeFormat('en-CA', { timeZone: 'Asia/Kolkata', year: 'numeric', month: '2-digit', day: '2-digit' }).formatToParts(new window.Date(e.detected_at));
        function datePart(k) { return localDate.filter(function (p) { return p.type === k; })[0].value; }
        var day = datePart('year') + '-' + datePart('month') + '-' + datePart('day');
        if (day < from.value || day > to.value) element('p', 'Selected event is outside the current date filters.', 'outside', detail);
        element('p', 'Video ID: ' + e.id + ' · Event ID: ' + e.event_id + ' · Alert ID: ' + e.alert_id, null, detail);
        element('p', 'Detected at (original): ' + e.detected_at + ' · Confidence: ' + e.confidence + ' · Duration: ' + e.duration_seconds + ' seconds · Size: ' + e.size_bytes + ' bytes · Provenance: real', null, detail);
        // Render original metadata as inert text, including unknown detector evidence fields.
        element('pre', JSON.stringify(e, null, 2), 'metadata', detail);
        var link = element('a', 'Event permalink', 'permalink', detail);
        var origin = new window.URL(window.location.href).origin;
        link.href = origin + '/?event_video=' + id + '#event-videos'; link.referrerPolicy = 'no-referrer';
        video = element('video', undefined, 'video', detail);
        video.controls = true; video.playsInline = true; video.preload = 'none'; video.referrerPolicy = 'no-referrer';
        video.setAttribute('referrerpolicy', 'no-referrer'); video.setAttribute('playsinline', '');
        playButton = element('button', 'Play / refresh playback', 'play', detail); playButton.type = 'button';
        playButton.addEventListener('click', playback);
        detailStatus = element('p', 'Click Play to load a short-lived private video URL, then use the video controls. No autoplay.', 'detail-status', detail);
        var thisVideo = video;
        video.addEventListener('error', function () {
          if (video !== thisVideo || !granted()) return;
          unload(); detailStatus.textContent = 'Playback unavailable or link expired. Click Play / refresh playback to retry.';
        });
      } catch (error) { if (current(g)) unavailable(error, detailStatus); }
      finally { if (current(g)) { busy = false; controls(); } }
    }
    async function playback() {
      if (!active || !granted() || !selected || !video) return;
      var g = invalidate(), id = selected;
      busy = true; controls(); detailStatus.textContent = 'Requesting private playback…';
      try {
        var body = await request('/api/event-videos/' + id + '/playback', g);
        if (!current(g)) return;
        var url = new window.URL(body && body.url);
        if (body.video_id !== id || body.expires_in !== 120 || url.protocol !== 'https:' ||
            !/^[0-9a-f]{32}\.r2\.cloudflarestorage\.com$/.test(url.hostname) || url.username || url.password || url.port || url.hash) throw fail();
        video.src = url.href; video.load();
        detailStatus.textContent = 'Ready. Use video controls to play. Private link expires in 120 seconds; click Play / refresh playback if needed.';
      } catch (error) { if (current(g)) { unload(); unavailable(error, detailStatus); } }
      finally { if (current(g)) { busy = false; controls(); } }
    }
    function filtersChanged() {
      invalidate(); page = 1; listOK = false; clearDetail(); cards.textContent = ''; viewButtons = [];
      pages.textContent = ''; status.textContent = 'Filters changed. Click Apply (inclusive dates, Asia/Kolkata).'; controls();
    }
    function mount() {
      if (section || !document.body) return;
      var style = element('style');
      style.textContent = '#hg-event-videos{max-width:1000px;margin:24px auto;padding:24px;background:#161b22;color:#e6edf3;border:1px solid #30363d;border-radius:14px;color-scheme:dark}#hg-event-videos[hidden],html:not([data-hg-auth="viewer"]) #hg-event-videos,html:not([data-hg-household="granted"]) #hg-event-videos{display:none!important}#hg-event-videos button,#hg-event-videos input{margin:6px;padding:8px;background:#21262d;color:#e6edf3;border:1px solid #484f58;border-radius:6px}#hg-event-videos button:disabled{opacity:.5}#hg-event-videos article{border:1px solid #30363d;padding:12px;margin:12px 0;border-radius:8px}#hg-event-videos pre{white-space:pre-wrap;overflow-wrap:anywhere}#hg-event-videos video{width:100%;max-height:440px}#hg-event-videos a{color:#9cc8ff}';
      document.head.appendChild(style);
      section = element('section'); section.id = 'hg-event-videos'; section.hidden = true; section.setAttribute('aria-label', 'Event videos');
      element('h2', 'Event videos', null, section);
      element('p', 'Real detection-triggered clips only. Synthetic scenarios are separate.', null, section);
      element('p', 'Inclusive detection dates · Asia/Kolkata (IST). Private playback links are temporary.', null, section);
      var filters = element('div', undefined, null, section);
      var fromLabel = element('label', 'From ', null, filters); from = element('input', undefined, 'from', fromLabel); from.type = 'date';
      var toLabel = element('label', 'To ', null, filters); to = element('input', undefined, 'to', toLabel); to.type = 'date';
      from.value = to.value = today();
      apply = element('button', 'Apply', 'apply', filters); todayButton = element('button', 'Today', 'today', filters); refresh = element('button', 'Refresh', 'refresh', filters);
      status = element('p', '', 'status', section); status.setAttribute('role', 'status'); status.setAttribute('aria-live', 'polite');
      cards = element('div', undefined, 'cards', section);
      var pagination = element('div', undefined, null, section);
      previous = element('button', 'Previous', 'previous', pagination); pages = element('span', '', 'pages', pagination); next = element('button', 'Next', 'next', pagination);
      detail = element('div', undefined, 'detail', section);
      [apply, todayButton, refresh, previous, next].forEach(function (b) { b.type = 'button'; });
      from.addEventListener('change', filtersChanged); to.addEventListener('change', filtersChanged);
      apply.addEventListener('click', function () { page = 1; loadList(); });
      todayButton.addEventListener('click', function () { from.value = to.value = today(); page = 1; loadList(); });
      refresh.addEventListener('click', function () { loadList(selected); });
      previous.addEventListener('click', function () { if (page > 1) { page -= 1; loadList(); } });
      next.addEventListener('click', function () { if (page < totalPages) { page += 1; loadList(); } });
      document.body.appendChild(section);
    }
    function deepLink() {
      var ids = new window.URL(window.location.href).searchParams.getAll('event_video');
      return ids.length === 1 && UUID.test(ids[0]) ? ids[0] : null;
    }
    function sync() {
      mount(); if (!section) return;
      if (!granted()) {
        active = false; invalidate(); clearDetail(); cards.textContent = ''; viewButtons = []; listOK = false;
        status.textContent = ''; pages.textContent = ''; section.hidden = true; controls(); return;
      }
      if (active) return;
      active = true; section.hidden = false; page = 1;
      var id = deepLink(); loadList(id);
      if (id) section.scrollIntoView({ block: 'start', behavior: 'smooth' });
    }
    observer = new window.MutationObserver(sync);
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-hg-auth', 'data-hg-household'] });
    document.addEventListener('DOMContentLoaded', sync);
    window.addEventListener('hg:session', sync);
    if (document.readyState !== 'loading') sync();
    return { sync: sync };
  }
  if (typeof module === 'object' && module.exports) module.exports = { buildUI: buildUI };
  else if (root && root.document) buildUI(root, root.document);
})(typeof window !== 'undefined' ? window : null);
