/*
 * VissLink — carte Lovelace des programmations horaires (v0.8, expérimental).
 *
 * Un seul fichier, sans dépendance ni compilation. N'utilise que l'objet
 * `hass` public (callService avec réponse, états) et des éléments HTML
 * standards : aucun composant interne de Home Assistant (ha-*), pour rester
 * compatible avec les mises à jour du frontend.
 *
 * La Vitotronic reste l'unique source de vérité : la carte n'affiche que ce
 * que l'ESP a lu (action visslink_schedule_read) et ne garde aucune
 * programmation. « Enregistré » n'apparaît qu'après la relecture exacte des
 * 8 octets par l'ESP.
 *
 * Configuration :
 *   type: custom:visslink-schedule-card
 *   device: iot_viessmann            # nom du nœud ESPHome (tirets -> _)
 *   status_entity: sensor.chaudiere_viessmann_programmations_etat_de_l_edition
 *   title: Programmations            # facultatif
 *   circuits: [a1, m2, ecs, bouclage]  # facultatif : ordre / filtre
 *
 * Installation : docs/programmations-v0.8.md.
 */

const VL_CARD_VERSION = "0.8.0";
const VL_DAYS = ["Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi", "Dimanche"];
const VL_DAYS_SHORT = ["Lu", "Ma", "Me", "Je", "Ve", "Sa", "Di"];
const VL_MAX_END = 23 * 60 + 50;

function vlEscape(text) {
  return String(text ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
}

function vlToMinutes(hhmm) {
  const m = /^(\d{1,2}):(\d{2})$/.exec(String(hhmm || "").trim());
  if (!m) return NaN;
  const h = Number(m[1]);
  const mi = Number(m[2]);
  if (h > 24 || mi > 59 || (h === 24 && mi !== 0)) return NaN;
  return h * 60 + mi;
}

function vlFromMinutes(minutes) {
  const h = Math.floor(minutes / 60);
  const m = minutes % 60;
  return `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}`;
}

function vlAge(seconds) {
  if (seconds === undefined || seconds === null) return "jamais lu";
  if (seconds < 90) return "à l'instant";
  const minutes = Math.round(seconds / 60);
  if (minutes < 90) return `il y a ${minutes} min`;
  const h = Math.floor(minutes / 60);
  const m = minutes % 60;
  return m ? `il y a ${h} h ${String(m).padStart(2, "0")}` : `il y a ${h} h`;
}

/** Mêmes règles que l'ESP (schedule_editor.h, parse_ranges). */
function vlValidate(ranges) {
  if (ranges.length > 4) return "4 plages au plus par jour";
  const parsed = [];
  for (const r of ranges) {
    const s = vlToMinutes(r.start);
    const e = vlToMinutes(r.end);
    if (Number.isNaN(s) || Number.isNaN(e)) return "heure incomplète";
    if (s % 10 || e % 10) return "pas de 10 minutes seulement (ex. 06:10, pas 06:15)";
    if (e <= s) return `${r.start}-${r.end} : la fin doit être après le début`;
    if (e > VL_MAX_END) return "fin au plus tard à 23:50";
    parsed.push([s, e]);
  }
  parsed.sort((a, b) => a[0] - b[0]);
  for (let i = 1; i < parsed.length; i++) {
    if (parsed[i][0] < parsed[i - 1][1]) return "plages qui se chevauchent";
  }
  return "";
}

function vlNormalize(ranges) {
  return ranges
    .map((r) => [vlToMinutes(r.start), vlToMinutes(r.end)])
    .sort((a, b) => a[0] - b[0])
    .map(([s, e]) => `${vlFromMinutes(s)}-${vlFromMinutes(e)}`)
    .join(",");
}

class VissLinkScheduleCard extends HTMLElement {
  setConfig(config) {
    if (!config || !config.device) {
      throw new Error("visslink-schedule-card : `device` (nom du nœud ESPHome, ex. iot_viessmann) est obligatoire");
    }
    this._config = { title: "Programmations", ...config };
    this._device = String(config.device).replace(/-/g, "_");
    this._circuit = (config.circuits && config.circuits[0]) || "a1";
    this._day = (new Date().getDay() + 6) % 7; // aujourd'hui (0 = lundi)
    this._data = null;
    this._draft = null; // plages en cours d'édition du jour choisi
    this._base = null; // référence (`expected`) au début de l'édition
    this._targets = new Set(); // autres jours auxquels appliquer
    this._sending = false;
    this._message = null; // {kind: "error"|"info", text}
    this._fetchedAt = 0;
    if (!this.shadowRoot) this.attachShadow({ mode: "open" });
    this._render();
  }

  set hass(hass) {
    const first = !this._hass;
    this._hass = hass;
    const entity = this._config.status_entity ? hass.states[this._config.status_entity] : undefined;
    const status = entity ? entity.state : undefined;
    if (first || status !== this._lastStatus) {
      this._lastStatus = status;
      this._load();
    }
  }

  connectedCallback() {
    // Rafraîchit l'affichage (âge des données) et suit une opération en cours.
    this._timer = setInterval(() => {
      if (this._data && this._data.busy) this._load();
      else if (Date.now() - this._fetchedAt > 60000) this._load();
    }, 5000);
  }

  disconnectedCallback() {
    clearInterval(this._timer);
  }

  getCardSize() {
    return 9;
  }

  static getStubConfig() {
    return { device: "iot_viessmann", status_entity: "" };
  }

  // --- Home Assistant -------------------------------------------------------

  _service(name) {
    return `${this._device}_visslink_schedule_${name}`;
  }

  async _call(name, data, withResponse = false) {
    return this._hass.callService("esphome", this._service(name), data, undefined, false, withResponse);
  }

  async _load() {
    if (!this._hass || this._loading) return;
    this._loading = true;
    try {
      const result = await this._call("read", { circuit: this._circuit }, true);
      const data = result && result.response ? result.response : result;
      this._fetchedAt = Date.now();
      this._apply(data);
    } catch (err) {
      this._message = { kind: "error", text: `Lecture impossible : ${this._errorText(err)}` };
    } finally {
      this._loading = false;
      this._render();
    }
  }

  _apply(data) {
    if (!data || !data.days) {
      this._data = data;
      return;
    }
    const previous = this._data;
    this._data = data;
    if (data.circuit && data.circuit !== this._circuit) this._circuit = data.circuit;
    if (this._draft !== null) {
      // Modification depuis la Vitotronic pendant l'édition : signalée, le
      // brouillon est gardé et l'ESP refusera de l'écrire sans rechargement.
      const day = data.days[this._day];
      const before = previous && previous.days ? previous.days[this._day] : undefined;
      if (before && day && before.raw !== day.raw && !(data.operation && data.operation.busy)) {
        const savedByUs = data.operation && data.operation.circuit === this._circuit && (data.operation.saved >> this._day) & 1;
        if (!savedByUs) {
          this._message = {
            kind: "error",
            text: `${VL_DAYS[this._day]} a été modifié depuis la Vitotronic pendant l'édition : « Abandonner » pour recharger.`,
          };
        }
      }
    }
  }

  _errorText(err) {
    if (!err) return "erreur inconnue";
    return err.message || err.error || (typeof err === "string" ? err : JSON.stringify(err));
  }

  // --- Actions de l'utilisateur ------------------------------------------------

  _dayRanges(day) {
    const d = this._data && this._data.days ? this._data.days[day] : undefined;
    if (!d || !d.known) return [];
    return (d.ranges || []).map((text) => {
      const [start, end] = text.split("-");
      return { start, end };
    });
  }

  _startEdit() {
    if (this._draft === null) {
      this._draft = this._dayRanges(this._day);
      this._base = this._data ? this._data.expected : "";
    }
  }

  _dirty() {
    if (this._draft === null) return false;
    return vlNormalize(this._draft) !== vlNormalize(this._dayRanges(this._day)) || this._draft.length !== this._dayRanges(this._day).length;
  }

  _discard() {
    this._draft = null;
    this._base = null;
    this._targets.clear();
    this._message = null;
  }

  _selectDay(day) {
    if (this._dirty() && day !== this._day) {
      this._message = { kind: "error", text: "Modifications non enregistrées : « Enregistrer » ou « Abandonner » d'abord." };
      this._render();
      return;
    }
    this._discard();
    this._day = day;
    this._render();
  }

  _selectCircuit(circuit) {
    if (this._dirty()) {
      this._message = { kind: "error", text: "Modifications non enregistrées : « Enregistrer » ou « Abandonner » d'abord." };
      this._render();
      return;
    }
    this._discard();
    this._circuit = circuit;
    this._data = null;
    this._render();
    this._load();
  }

  async _send(name, data, okText) {
    this._sending = true;
    this._message = { kind: "info", text: "Demande envoyée à l'ESP…" };
    this._render();
    try {
      await this._call(name, data);
      this._message = { kind: "info", text: okText };
      return true;
    } catch (err) {
      this._message = { kind: "error", text: `Refusé : ${this._errorText(err)}` };
      return false;
    } finally {
      this._sending = false;
      await this._load();
    }
  }

  async _save() {
    const error = this._draft !== null ? vlValidate(this._draft) : "";
    if (error) {
      this._message = { kind: "error", text: error };
      this._render();
      return;
    }
    const days = [this._day, ...this._targets].sort((a, b) => a - b);
    let ok;
    if (!this._dirty() && this._targets.size) {
      // Copie exacte des octets lus du jour choisi.
      ok = await this._send(
        "copy",
        { circuit: this._circuit, source: this._day, days: [...this._targets].join(","), expected: this._data.expected },
        "Copie acceptée : écriture et relecture jour par jour en cours…",
      );
    } else {
      ok = await this._send(
        "set",
        { circuit: this._circuit, days: days.join(","), ranges: vlNormalize(this._draft || this._dayRanges(this._day)), expected: this._base || this._data.expected },
        "Modification acceptée : écriture et relecture en cours…",
      );
    }
    if (ok) {
      this._draft = null;
      this._base = null;
      this._targets.clear();
      this._render();
    }
  }

  // --- Rendu ---------------------------------------------------------------------

  _dayState(day) {
    const op = this._data && this._data.operation;
    if (!op || op.circuit !== this._circuit || !((op.requested >> day) & 1)) return null;
    if ((op.saved >> day) & 1) return { cls: "ok", text: "enregistré (relu)" };
    if ((op.unchanged >> day) & 1) return { cls: "ok", text: "inchangé" };
    if (op.failed_day === day) return { cls: "err", text: "échec" };
    if ((op.not_done >> day) & 1) return { cls: "err", text: "non enregistré" };
    if (op.busy && op.current_day === day) return { cls: "busy", text: "écriture en cours" };
    if (op.busy) return { cls: "busy", text: "en attente" };
    return null;
  }

  _bar(ranges) {
    return ranges
      .map((r) => {
        const s = vlToMinutes(r.start);
        const e = vlToMinutes(r.end);
        if (Number.isNaN(s) || Number.isNaN(e) || e <= s) return "";
        return `<span class="seg" style="left:${(s / 1440) * 100}%;width:${((e - s) / 1440) * 100}%" title="${vlEscape(r.start)}-${vlEscape(r.end)}"></span>`;
      })
      .join("");
  }

  _render() {
    if (!this.shadowRoot || !this._config) return;
    const data = this._data;
    const busy = (data && data.busy) || this._sending;
    const dirty = this._dirty();
    const circuits = (data && data.circuits) || [];
    const wanted = this._config.circuits;
    const tabs = (wanted ? wanted.map((k) => circuits.find((c) => c.key === k)).filter(Boolean) : circuits)
      .map((c) => `<button class="tab ${c.key === this._circuit ? "on" : ""}" data-circuit="${vlEscape(c.key)}">${vlEscape(c.label)}</button>`)
      .join("");

    let rows = "";
    let oldest;
    if (data && data.days) {
      rows = data.days
        .map((d) => {
          if (d.known && (oldest === undefined || d.age_s > oldest)) oldest = d.age_s;
          const ranges = d.day === this._day && this._draft !== null ? this._draft : this._dayRanges(d.day);
          const state = this._dayState(d.day);
          const label = d.known ? (ranges.length ? ranges.map((r) => `${r.start}-${r.end}`).join(", ") : "aucune plage") : "non lu";
          return `<button class="row ${d.day === this._day ? "sel" : ""} ${d.known ? "" : "unknown"}" data-day="${d.day}">
            <span class="dn">${VL_DAYS_SHORT[d.day]}</span>
            <span class="track">${this._bar(ranges)}${d.day === this._day && dirty ? '<span class="draft-flag">brouillon</span>' : ""}</span>
            <span class="txt">${vlEscape(label)}${d.locked ? ' <em class="lock">bouclage temporaire</em>' : ""}
              ${state ? `<em class="st ${state.cls}">${state.text}</em>` : ""}</span>
          </button>`;
        })
        .join("");
    }

    const day = data && data.days ? data.days[this._day] : undefined;
    const editable = day && day.known && !day.locked && data.writes_enabled;
    const ranges = this._draft !== null ? this._draft : this._dayRanges(this._day);
    const validation = this._draft !== null ? vlValidate(this._draft) : "";
    const editor = !day
      ? ""
      : `<div class="editor">
        <div class="ed-head"><strong>${VL_DAYS[this._day]}</strong>
          ${dirty ? '<span class="badge warn">Modifications non enregistrées</span>' : ""}
          ${day.known ? `<span class="age">lu ${vlAge(day.age_s)}</span>` : '<span class="badge warn">pas encore lu : rafraîchir</span>'}
        </div>
        ${
          day.locked
            ? '<p class="note">Plage temporaire du bouclage en cours : modification possible après la restauration.</p>'
            : ""
        }
        <div class="ranges">
          ${ranges
            .map(
              (r, i) => `<div class="range">
              <input type="time" step="600" data-i="${i}" data-k="start" value="${vlEscape(r.start)}" ${editable && !busy ? "" : "disabled"} aria-label="Début plage ${i + 1}">
              <span>→</span>
              <input type="time" step="600" data-i="${i}" data-k="end" value="${vlEscape(r.end)}" ${editable && !busy ? "" : "disabled"} aria-label="Fin plage ${i + 1}">
              <button class="icon" data-remove="${i}" ${editable && !busy ? "" : "disabled"} title="Supprimer la plage">✕</button>
            </div>`,
            )
            .join("")}
          ${ranges.length === 0 ? '<p class="note">Aucune plage : circuit en mode réduit / arrêt toute la journée.</p>' : ""}
          <button class="link" data-act="add" ${editable && !busy && ranges.length < 4 ? "" : "disabled"}>+ Ajouter une plage (${4 - ranges.length} libre${4 - ranges.length > 1 ? "s" : ""})</button>
        </div>
        ${validation ? `<p class="err">${vlEscape(validation)}</p>` : ""}
        <div class="copy">
          <span>Appliquer aussi à :</span>
          ${VL_DAYS_SHORT.map((n, i) =>
            i === this._day
              ? ""
              : `<label class="chip ${this._targets.has(i) ? "on" : ""} ${data.days[i].locked ? "dis" : ""}"><input type="checkbox" data-target="${i}" ${this._targets.has(i) ? "checked" : ""} ${editable && !busy && !data.days[i].locked ? "" : "disabled"}>${n}</label>`,
          ).join("")}
          <button class="link" data-act="weekdays" ${editable && !busy ? "" : "disabled"}>Lu→Ve</button>
          <button class="link" data-act="all" ${editable && !busy ? "" : "disabled"}>Tous</button>
          <button class="link" data-act="none" ${this._targets.size ? "" : "disabled"}>Aucun</button>
        </div>
        <div class="actions">
          <button class="secondary" data-act="clear" ${editable && !busy && ranges.length ? "" : "disabled"}>Effacer la journée</button>
          <button class="secondary" data-act="discard" ${this._draft !== null || this._targets.size ? "" : "disabled"}>Abandonner</button>
          <button class="primary" data-act="save" ${editable && !busy && !validation && (dirty || this._targets.size) ? "" : "disabled"}>
            ${busy ? "Écriture en cours…" : this._targets.size ? `Enregistrer (${this._targets.size + (dirty ? 1 : 0)} jour${this._targets.size + (dirty ? 1 : 0) > 1 ? "s" : ""})` : "Enregistrer"}
          </button>
        </div>
      </div>`;

    const status = (this._config.status_entity && this._hass && this._hass.states[this._config.status_entity]?.state) || (data && data.status) || "";
    const undo = data && data.undo && data.undo.circuit === this._circuit && !busy
      ? `<button class="link" data-act="undo">Annuler la dernière modification (${VL_DAYS_SHORT[data.undo.day]})</button>`
      : "";
    const warnings = [];
    if (data && data.writes_enabled === false) warnings.push("Écritures de programmation désactivées sur l'ESP (lecture seule).");
    if (data && data.link_ok === false) warnings.push("Liaison Optolink non synchronisée.");

    this.shadowRoot.innerHTML = `<style>${VL_STYLE}</style>
      <div class="card">
        <div class="head">
          <span class="title">${vlEscape(this._config.title)}</span>
          <span class="age">${data && data.days ? `données lues ${vlAge(oldest)}` : ""}</span>
          <button class="icon" data-act="refresh" title="Relire les 7 jours depuis la Vitotronic (~3 min)" ${busy ? "disabled" : ""}>⟳</button>
        </div>
        <div class="tabs">${tabs}</div>
        ${warnings.map((w) => `<p class="err">${vlEscape(w)}</p>`).join("")}
        <div class="grid">
          <div class="hours"><span></span><span class="scale">${[0, 6, 12, 18, 24].map((h) => `<i style="left:${(h / 24) * 100}%">${h}</i>`).join("")}</span><span></span></div>
          ${rows || `<p class="note">${this._loading ? "Lecture…" : "Aucune donnée (ESP joignable ? `device` correct ?)"}</p>`}
        </div>
        ${editor}
        ${this._message ? `<p class="msg ${this._message.kind}">${vlEscape(this._message.text)}</p>` : ""}
        <div class="foot"><span class="status ${busy ? "busy" : ""}">${vlEscape(status)}</span>${undo}</div>
      </div>`;
    this._bind();
  }

  _bind() {
    const root = this.shadowRoot;
    root.querySelectorAll("[data-circuit]").forEach((b) => b.addEventListener("click", () => this._selectCircuit(b.dataset.circuit)));
    root.querySelectorAll("[data-day]").forEach((b) => b.addEventListener("click", () => this._selectDay(Number(b.dataset.day))));
    root.querySelectorAll("input[type=time]").forEach((input) =>
      input.addEventListener("change", () => {
        this._startEdit();
        this._draft[Number(input.dataset.i)][input.dataset.k] = input.value;
        this._message = null;
        this._render();
      }),
    );
    root.querySelectorAll("[data-remove]").forEach((b) =>
      b.addEventListener("click", () => {
        this._startEdit();
        this._draft.splice(Number(b.dataset.remove), 1);
        this._render();
      }),
    );
    root.querySelectorAll("[data-target]").forEach((c) =>
      c.addEventListener("change", () => {
        const d = Number(c.dataset.target);
        if (c.checked) this._targets.add(d);
        else this._targets.delete(d);
        this._render();
      }),
    );
    root.querySelectorAll("[data-act]").forEach((b) => b.addEventListener("click", () => this._action(b.dataset.act)));
  }

  _action(act) {
    const days = this._data && this._data.days ? this._data.days : [];
    const pick = (list) => {
      this._targets = new Set(list.filter((d) => d !== this._day && days[d] && !days[d].locked));
      this._render();
    };
    switch (act) {
      case "add": {
        this._startEdit();
        const last = this._draft.length ? vlToMinutes(this._draft[this._draft.length - 1].end) : 360;
        const start = Math.min(Number.isNaN(last) ? 360 : last, VL_MAX_END - 60);
        this._draft.push({ start: vlFromMinutes(start), end: vlFromMinutes(Math.min(start + 60, VL_MAX_END)) });
        this._render();
        break;
      }
      case "clear":
        this._startEdit();
        this._draft = [];
        this._render();
        break;
      case "discard":
        this._discard();
        this._render();
        this._load();
        break;
      case "weekdays":
        pick([0, 1, 2, 3, 4]);
        break;
      case "all":
        pick([0, 1, 2, 3, 4, 5, 6]);
        break;
      case "none":
        pick([]);
        break;
      case "save":
        this._save();
        break;
      case "refresh":
        this._send("refresh", { circuit: this._circuit }, "Relecture des 7 jours demandée (environ 3 min, un jour toutes les 30 s).");
        break;
      case "undo":
        this._send("undo", {}, "Annulation acceptée : écriture et relecture en cours…");
        break;
      default:
        break;
    }
  }
}

const VL_STYLE = `
  :host { display: block; }
  .card { display: block; padding: 12px 16px 16px; color: var(--primary-text-color, #212121);
    background: var(--ha-card-background, var(--card-background-color, #fff)); border-radius: var(--ha-card-border-radius, 12px);
    box-shadow: var(--ha-card-box-shadow, none); border: 1px solid var(--divider-color, #e0e0e0); }
  button { font: inherit; color: inherit; cursor: pointer; }
  button:disabled { cursor: default; opacity: .45; }
  .head { display: flex; align-items: center; gap: 8px; }
  .title { font-size: 1.15em; font-weight: 600; flex: 1; }
  .age { font-size: .85em; color: var(--secondary-text-color, #727272); }
  .icon { border: none; background: none; font-size: 1.1em; padding: 6px; min-width: 36px; min-height: 36px; border-radius: 50%; }
  .icon:hover:not(:disabled) { background: var(--secondary-background-color, #f2f2f2); }
  .tabs { display: flex; flex-wrap: wrap; gap: 6px; margin: 8px 0; }
  .tab { border: 1px solid var(--divider-color, #ccc); background: none; border-radius: 16px; padding: 6px 12px; min-height: 36px; }
  .tab.on { background: var(--primary-color, #03a9f4); color: var(--text-primary-color, #fff); border-color: transparent; }
  .grid { display: flex; flex-direction: column; gap: 4px; }
  .hours, .row { display: grid; grid-template-columns: 2.2em minmax(0, 1fr); gap: 8px; align-items: center; }
  .hours > span:last-child { display: none; }
  .scale { position: relative; height: 1.1em; font-size: .75em; color: var(--secondary-text-color, #727272); }
  .scale i { position: absolute; transform: translateX(-50%); font-style: normal; }
  .scale i:first-child { transform: none; } .scale i:last-child { transform: translateX(-100%); }
  .row { border: 1px solid transparent; background: none; text-align: left; padding: 4px; border-radius: 8px; min-height: 44px;
    grid-template-areas: "dn track" "dn txt"; row-gap: 2px; }
  .row:hover { background: var(--secondary-background-color, #f5f5f5); }
  .row.sel { border-color: var(--primary-color, #03a9f4); }
  .row.unknown .track { background-image: repeating-linear-gradient(45deg, transparent 0 6px, var(--divider-color, #ddd) 6px 8px); }
  .dn { grid-area: dn; font-weight: 600; }
  .track { grid-area: track; position: relative; height: 14px; border-radius: 4px; background: var(--secondary-background-color, #eee); overflow: hidden; }
  .seg { position: absolute; top: 0; bottom: 0; background: var(--state-climate-heat-color, var(--accent-color, #ff9800)); border-radius: 3px; }
  .draft-flag { position: absolute; right: 4px; top: -1px; font-size: 10px; color: var(--warning-color, #b26a00); }
  .txt { grid-area: txt; font-size: .85em; color: var(--secondary-text-color, #555); }
  em { font-style: normal; font-size: .9em; padding: 0 6px; border-radius: 8px; margin-left: 4px; white-space: nowrap; }
  .lock { background: var(--warning-color, #ffa600); color: #000; }
  .st.ok { background: var(--success-color, #43a047); color: #fff; }
  .st.err { background: var(--error-color, #db4437); color: #fff; }
  .st.busy { background: var(--info-color, #039be5); color: #fff; }
  .editor { margin-top: 12px; padding-top: 12px; border-top: 1px solid var(--divider-color, #e0e0e0); }
  .ed-head { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; }
  .badge { font-size: .8em; padding: 2px 8px; border-radius: 10px; }
  .badge.warn { background: var(--warning-color, #ffa600); color: #000; }
  .ranges { display: flex; flex-direction: column; gap: 6px; margin: 8px 0; }
  .range { display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
  input[type=time] { font: inherit; padding: 6px; min-height: 36px; border: 1px solid var(--divider-color, #ccc); border-radius: 6px;
    background: var(--card-background-color, #fff); color: inherit; }
  .link { border: none; background: none; color: var(--primary-color, #03a9f4); padding: 6px 4px; min-height: 36px; text-align: left; }
  .copy { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; margin: 8px 0; font-size: .9em; }
  .chip { display: inline-flex; align-items: center; gap: 2px; border: 1px solid var(--divider-color, #ccc); border-radius: 14px; padding: 4px 8px; min-height: 28px; cursor: pointer; }
  .chip.on { border-color: var(--primary-color, #03a9f4); }
  .chip.dis { opacity: .45; }
  .chip input { margin: 0 2px 0 0; }
  .actions { display: flex; flex-wrap: wrap; justify-content: flex-end; gap: 8px; }
  .primary, .secondary { border-radius: 8px; padding: 8px 14px; min-height: 40px; }
  .primary { background: var(--primary-color, #03a9f4); color: var(--text-primary-color, #fff); border: none; }
  .secondary { background: none; border: 1px solid var(--divider-color, #ccc); }
  .note { font-size: .85em; color: var(--secondary-text-color, #727272); margin: 4px 0; }
  .err, .msg.error { color: var(--error-color, #db4437); font-size: .9em; margin: 6px 0; }
  .msg.info { color: var(--primary-color, #03a9f4); font-size: .9em; margin: 6px 0; }
  .foot { display: flex; flex-wrap: wrap; justify-content: space-between; gap: 8px; margin-top: 8px; font-size: .85em;
    color: var(--secondary-text-color, #727272); }
  .status.busy::before { content: "● "; color: var(--info-color, #039be5); }
  @media (max-width: 420px) { .card { padding: 10px 10px 14px; } .actions button { flex: 1 1 auto; } }
`;

customElements.define("visslink-schedule-card", VissLinkScheduleCard);
window.customCards = window.customCards || [];
window.customCards.push({
  type: "visslink-schedule-card",
  name: "VissLink — Programmations",
  description: "Programmations horaires de la Vitotronic (VissLink v0.8), lues et écrites par l'ESP.",
});
console.info(`%c VISSLINK-SCHEDULE-CARD %c ${VL_CARD_VERSION} `, "background:#03a9f4;color:#fff", "");
