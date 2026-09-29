// ============================================================
// 5/10/15 - PebbleKit JS bridge
// Opens the hosted config page (index.html on GitHub Pages)
// and ferries the library to the watch via AppMessage.
// ============================================================

// CONFIG PAGE URL - your GitHub Pages URL, with trailing slash.
var CONFIG_URL = 'https://thefish30.github.io/5-10-15-config/';

var STORAGE_KEY = 'library_json';

// ------------------------------------------------------------
// When the watch app asks to open settings, open the hosted
// page. ALWAYS append a fragment: the stored library if we have
// one, or the marker "pebble" on first-ever open. The page uses
// the presence of any fragment to know it's running inside the
// Pebble app (vs. a plain browser).
// ------------------------------------------------------------
Pebble.addEventListener('showConfiguration', function() {
  var current = localStorage.getItem(STORAGE_KEY) || '';
  var url = CONFIG_URL + '#' + (current ? encodeURIComponent(current) : 'pebble');
  Pebble.openURL(url);
});

// ------------------------------------------------------------
// When the config page closes (user tapped Done), it returns
// the library as JSON via pebblejs://close#<payload>. We stash
// it for next time and flatten it into the compact wire string
// the watch parses.
//
// Wire format (single string):
//   listCount ~ list1 ~ list2 ~ ...
// each list:
//   name | loops(0/1) | taskCount | t1 | t2 | ... | thenRun
// each task:
//   name ^ minutes ^ colorIndex
// thenRun: 0 = none, else target list index + 1. Trailing field,
// so older watch builds simply ignore it.
// ------------------------------------------------------------
Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) {
    return;  // user backed out without tapping Done
  }

  var library;
  try {
    library = JSON.parse(decodeURIComponent(e.response));
  } catch (err) {
    console.log('5/10/15: could not parse config response: ' + err);
    return;
  }

  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(library));
  } catch (err) { /* ignore storage errors */ }

  var wire = flattenLibrary(library);
  sendToWatch(wire);
});

// ------------------------------------------------------------
// Flatten the library array into the compact wire string.
// ------------------------------------------------------------
function flattenLibrary(library) {
  var listCount = library.length;
  var parts = [String(listCount)];

  // Chains are stored by list name in the config page (survives
  // reordering); resolve to an index here. First match wins.
  var nameToIndex = {};
  for (var k = 0; k < library.length; k++) {
    if (!nameToIndex.hasOwnProperty(library[k].name)) {
      nameToIndex[library[k].name] = k;
    }
  }

  for (var i = 0; i < library.length; i++) {
    var l = library[i];
    var taskCount = l.tasks.length;
    var listFields = [
      sanitize(l.name),
      l.loops ? '1' : '0',
      String(taskCount)
    ];
    for (var j = 0; j < l.tasks.length; j++) {
      var t = l.tasks[j];
      var taskStr = sanitize(t.name) + '^' + String(t.minutes) + '^' + String(t.color);
      listFields.push(taskStr);
    }
    var thenRun = 0;
    if (!l.loops && l.thenRun && nameToIndex.hasOwnProperty(l.thenRun)) {
      var target = nameToIndex[l.thenRun];
      if (target !== i) thenRun = target + 1;
    }
    listFields.push(String(thenRun));
    parts.push(listFields.join('|'));
  }

  return parts.join('~');
}

// Strip any delimiter characters from user text so they can't break parsing.
function sanitize(s) {
  return String(s).replace(/[~|^]/g, ' ').substring(0, 19);
}

// ------------------------------------------------------------
// Send the wire string to the watch via AppMessage.
// Message key must be registered in CloudPebble Settings as
// LIBRARY_DATA (main.c reads it via MESSAGE_KEY_LIBRARY_DATA).
// ------------------------------------------------------------
function sendToWatch(wire) {
  Pebble.sendAppMessage(
    { 'LIBRARY_DATA': wire },
    function() { console.log('5/10/15: library sent to watch (' + wire.length + ' bytes).'); },
    function(e) { console.log('5/10/15: send failed: ' + JSON.stringify(e)); }
  );
}

Pebble.addEventListener('ready', function() {
  console.log('5/10/15: PebbleKit JS ready.');
});