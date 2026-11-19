// Test headless de lovelace/visslink-schedule-card.js (Node, sans dépendance) :
// DOM minimal simulé, `hass` factice. Lancé par tests/test_lovelace_card.py.
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const assert = require("assert");
// Objets créés dans le contexte vm : comparaison par JSON.
const same = (a, b) => assert.strictEqual(JSON.stringify(a), JSON.stringify(b));

const registry = {};
class FakeShadow {
  constructor() { this.innerHTML = ""; }
  querySelectorAll() { return []; }
}
class FakeHTMLElement {
  attachShadow() { this.shadowRoot = new FakeShadow(); return this.shadowRoot; }
}
const sandbox = {
  HTMLElement: FakeHTMLElement,
  customElements: { define: (name, cls) => { registry[name] = cls; } },
  window: {},
  console: { info() {}, log: console.log },
  setInterval: () => 1,
  clearInterval: () => {},
  Date,
};
sandbox.window = sandbox;
const src = fs.readFileSync(path.join(__dirname, "../../lovelace/visslink-schedule-card.js"), "utf8");
vm.runInNewContext(src + "\n;this.vlValidate = vlValidate; this.vlNormalize = vlNormalize; this.vlAge = vlAge;", sandbox);

const { vlValidate, vlNormalize, vlAge } = sandbox;
const R = (s, e) => ({ start: s, end: e });

// Mêmes règles que l'ESP.
assert.strictEqual(vlValidate([]), "");
assert.strictEqual(vlValidate([R("06:00", "08:30"), R("17:00", "22:00")]), "");
assert.strictEqual(vlValidate([R("06:00", "08:00"), R("08:00", "09:00")]), "");  // contiguës admises
assert.strictEqual(vlValidate([R("06:00", "08:00"), R("07:50", "09:00")]), "plages qui se chevauchent");
assert.strictEqual(vlValidate([R("06:15", "08:00")]), "pas de 10 minutes seulement (ex. 06:10, pas 06:15)");
assert.strictEqual(vlValidate([R("22:00", "24:00")]), "fin au plus tard à 23:50");
assert.match(vlValidate([R("09:00", "08:00")]), /la fin doit être après le début/);
assert.strictEqual(vlValidate([R("01:00", "02:00"), R("03:00", "04:00"), R("05:00", "06:00"), R("07:00", "08:00"), R("09:00", "10:00")]),
  "4 plages au plus par jour");
assert.strictEqual(vlValidate([R("", "08:00")]), "heure incomplète");
assert.strictEqual(vlNormalize([R("17:00", "22:00"), R("6:00", "08:30")]), "06:00-08:30,17:00-22:00");
assert.strictEqual(vlAge(undefined), "jamais lu");
assert.strictEqual(vlAge(7500), "il y a 2 h 05");

// Données renvoyées par visslink_schedule_read.
function readResponse(over = {}) {
  const days = [0, 1, 2, 3, 4, 5, 6].map((d) => ({
    day: d, label: ["Lu", "Ma", "Me", "Je", "Ve", "Sa", "Di"][d], known: true, locked: false,
    raw: d < 5 ? "30B0FFFFFFFFFFFF" : "31438CA4FFFFFFFF", age_s: 120,
    ranges: d < 5 ? ["06:00-22:00"] : ["06:10-08:30", "17:30-20:30"],
  }));
  return {
    circuits: [{ key: "a1", label: "Radiateurs" }, { key: "m2", label: "Plancher" }],
    circuit: "a1", label: "Radiateurs", expected: "EXPECTED-WEEK", days,
    busy: false, status: "Prêt", link_ok: true, max_end: "23:50", max_ranges: 4, ...over,
  };
}

async function main() {
  const Card = registry["visslink-schedule-card"];
  assert.ok(Card, "élément enregistré");
  assert.ok(sandbox.customCards.some((c) => c.type === "visslink-schedule-card"));
  assert.throws(() => new Card().setConfig({}), /device/);

  const calls = [];
  let response = readResponse();
  let fail = null;
  const hass = {
    states: { "sensor.etat": { state: "Prêt" } },
    callService: async (domain, service, data, target, notify, withResponse) => {
      calls.push({ domain, service, data, withResponse });
      if (service.endsWith("_read")) return { context: {}, response };
      if (fail) throw { message: fail };
      return { context: {} };
    },
  };
  const card = new Card();
  card.setConfig({ device: "iot-viessmann", status_entity: "sensor.etat" });
  card._day = 0;
  card.hass = hass;
  await new Promise((r) => setImmediate(r));
  assert.strictEqual(calls[0].service, "iot_viessmann_visslink_schedule_read");
  assert.strictEqual(calls[0].withResponse, true);
  same(calls[0].data, { circuit: "a1" });
  let html = card.shadowRoot.innerHTML;
  assert.match(html, /Radiateurs/);
  assert.match(html, /06:00-22:00/);
  assert.match(html, /données lues il y a 2 min/);
  assert.doesNotMatch(html, /Modifications non enregistrées/);
  assert.doesNotMatch(html, /<ha-/);  // aucun élément interne de Home Assistant

  // Édition : brouillon signalé, rien n'est envoyé avant « Enregistrer ».
  card._startEdit();
  card._draft[0].start = "05:30";
  card._render();
  html = card.shadowRoot.innerHTML;
  assert.match(html, /Modifications non enregistrées/);
  assert.match(html, /brouillon/);
  const before = calls.length;
  // Changement de jour refusé tant que le brouillon n'est pas enregistré.
  card._selectDay(3);
  assert.strictEqual(card._day, 0);
  assert.match(card.shadowRoot.innerHTML, /Modifications non enregistrées : « Enregistrer »/);
  assert.strictEqual(calls.length, before);

  // Enregistrer lundi + mardi, mercredi : action `set`, référence affichée.
  card._targets.add(1);
  card._targets.add(2);
  response = readResponse({
    busy: true, status: "Radiateurs : écriture Ma (2/3) · enregistrés : Lu",
    operation: { circuit: "a1", busy: true, current_day: 1, requested: 7, saved: 1, unchanged: 0, not_done: 0, failed_day: -1 },
  });
  await card._save();
  const set = calls.find((c) => c.service.endsWith("_set"));
  same(set.data, { circuit: "a1", days: "0,1,2", ranges: "05:30-22:00", expected: "EXPECTED-WEEK" });
  html = card.shadowRoot.innerHTML;
  assert.match(html, /enregistré \(relu\)/);   // lundi : relu
  assert.match(html, /écriture en cours/);      // mardi
  assert.match(html, /en attente/);             // mercredi
  assert.match(html, /Écriture en cours…/);
  assert.strictEqual(card._draft, null);

  // Échec au milieu : jours non enregistrés affichés.
  response = readResponse({
    status: "Échec Radiateurs Ma : ...",
    operation: { circuit: "a1", busy: false, current_day: -1, requested: 7, saved: 1, unchanged: 0, not_done: 4, failed_day: 1 },
  });
  await card._load();
  html = card.shadowRoot.innerHTML;
  assert.match(html, /échec/);
  assert.match(html, /non enregistré/);
  assert.doesNotMatch(html, /écriture en cours/);

  // Copie sans modification : action `copy` (octets exacts du jour lu).
  card._discard();
  card._day = 0;
  card._targets = new Set([5, 6]);
  await card._save();
  const copy = calls.filter((c) => c.service.endsWith("_copy")).pop();
  same(copy.data, { circuit: "a1", source: 0, days: "5,6", expected: "EXPECTED-WEEK" });

  // Refus de l'ESP affiché tel quel.
  fail = "Radiateurs Ma modifié depuis l'affichage (panneau ?) : programmation rechargée, recommencer";
  card._startEdit();
  card._draft = [R("07:00", "21:00")];
  await card._save();
  assert.match(card.shadowRoot.innerHTML, /Refusé : Radiateurs Ma modifié depuis l&#39;affichage/);
  assert.notStrictEqual(card._draft, null);  // brouillon gardé
  fail = null;

  // Validation locale : jamais envoyé.
  const n = calls.length;
  card._draft = [R("06:00", "08:00"), R("07:00", "09:00")];
  await card._save();
  assert.strictEqual(calls.length, n);
  assert.match(card.shadowRoot.innerHTML, /plages qui se chevauchent/);
  card._discard();

  // Jour verrouillé par le bouclage temporaire : non modifiable.
  response = readResponse();
  response.days[6].locked = true;
  card._day = 6;
  await card._load();
  html = card.shadowRoot.innerHTML;
  assert.match(html, /bouclage temporaire/);
  assert.match(html, /modification possible après la restauration/);

  // Jour pas encore lu, liaison absente.
  response = readResponse({ link_ok: false });
  response.days[2] = { day: 2, label: "Me", known: false, locked: false };
  card._day = 2;
  await card._load();
  html = card.shadowRoot.innerHTML;
  assert.match(html, /pas encore lu : rafraîchir/);
  assert.doesNotMatch(html, /Écritures de programmation désactivées/);
  assert.match(html, /Liaison Optolink non synchronisée/);

  // Rafraîchissement manuel et annulation.
  card._action("refresh");
  await new Promise((r) => setImmediate(r));
  same(calls.filter((c) => c.service.endsWith("_refresh")).pop().data, { circuit: "a1" });
  response = readResponse({ undo: { circuit: "a1", day: 4, original: "x", target: "y" } });
  await card._load();
  assert.match(card.shadowRoot.innerHTML, /Annuler la dernière modification \(Ve\)/);

  // Nouvel état de l'entité : relecture.
  const reads = calls.filter((c) => c.service.endsWith("_read")).length;
  hass.states["sensor.etat"] = { state: "Enregistré et relu : Radiateurs Lu" };
  card.hass = hass;
  await new Promise((r) => setImmediate(r));
  assert.strictEqual(calls.filter((c) => c.service.endsWith("_read")).length, reads + 1);
  console.log("card tests: ok");
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
