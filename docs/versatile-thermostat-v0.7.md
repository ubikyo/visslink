# VissLink et Versatile Thermostat (v0.7)

Ce document décrit comment les entités VissLink des circuits A1 (radiateurs)
et M2 (plancher chauffant) peuvent cohabiter avec les thermostats
Versatile Thermostat (VTherm) existants. **Il ne modifie aucun VTherm
existant.** Les exemples utilisent des identifiants à remplacer, notés
`<…>` : relever les vrais dans *Paramètres > Appareils et services*.

## 1. Deux couches qui ne se recouvrent pas

| Couche | Rôle | Ne fait jamais |
|---|---|---|
| **Vitotronic 300 KW3** | loi d'eau, vanne mélangeuse M2, départ maximal, circulateurs, ECS, hors-gel | — |
| **VissLink** (ESPHome) | lit et, si autorisé, modifie les **consignes de circuit** A1/M2 et leurs programmes ; lit températures et circulateurs | piloter une pièce, une pompe, la chaudière ou la vanne |
| **VTherm** (Home Assistant) | régule **chaque pièce** par ses têtes thermostatiques (TRV) | écrire dans la Vitotronic, sauf via les entités VissLink et par une décision explicite (§ 5) |

## 2. Correspondance des entités

| Circuit | Registre | Entité VissLink (nom de production) | Nature |
|---|---|---|---|
| A1 radiateurs | `0x2306` | `number` « Radiateurs - Consigne confort » | consigne d'ambiance **du circuit** |
| A1 | `0x2307` | `number` « Radiateurs - Consigne réduite » | idem, période réduite |
| A1 | `0x2301` | `select` « Radiateurs - Mode actuel » | programme (ECS seule / Chauffage + ECS / Arrêt) |
| M2 plancher | `0x3306` | `number` « Plancher - Consigne confort » | consigne d'ambiance du circuit |
| M2 | `0x3307` | `number` « Plancher - Consigne réduite » | idem, période réduite |
| M2 | `0x3301` | `select` « Plancher chauffant - Mode actuel » | programme |
| M2 | `0x3900` | `sensor` « Température départ M2 » | eau du plancher, **pas** une ambiance |
| M2 | `0x0898` | `sensor` « Plancher chauffant - Température ambiante (Vitotrol) » | ambiance ; observée cohérente avec la Vitotrol, **pas encore validée** |
| M2 | `0x3906` | `binary_sensor` « Plancher chauffant - Circulateur » | état ; remonté, **pas encore validé** |
| M2 | — | `climate` « Plancher chauffant » (essai v0.7) | agrégat des lignes M2 ci-dessus |

## 3. Thermostat de pièce ou consigne de circuit

| | Thermostat de pièce (VTherm sur TRV) | Consigne de circuit (Vitotronic) |
|---|---|---|
| Mesure | sonde de la pièce | aucune en régulation climatique pure ; la Vitotrol de M2 peut corriger si l'influence d'ambiance est codée |
| Effet | ouvre ou ferme **une** vanne | déplace la **courbe de chauffe** : la température de départ de **tout** le circuit |
| Dynamique | minutes | radiateurs : dizaines de minutes ; plancher : **heures** |
| Erreur typique | pièce trop chaude ou trop froide | toutes les pièces du circuit décalées, chaudière plus sollicitée |

Une consigne de circuit à 21 °C ne garantit pas 21 °C dans une pièce. Elle
règle le niveau d'eau envoyé à toutes les pièces. Le climate M2 affiche
cette consigne comme **cible** et l'ambiance Vitotrol comme température
**actuelle**. Ce n'est pas un thermostat de pièce.

## 4. Approches étudiées

### A. Quatre `number` + entités de mode existantes (implémentée, production)

- Simple et explicite, sans ambiguïté sur ce que représente chaque valeur.
- Utilisable dans des automatisations lentes (§ 6) et dans des cartes.
- Ne peut pas servir de sous-jacent à un VTherm.

### B. Deux `climate` A1 et M2

| | Climate A1 | Climate M2 |
|---|---|---|
| Intérêt | faible : chaque pièce a déjà son VTherm sur TRV | réel : le plancher n'a pas de TRV, la Vitotrol mesure l'ambiance |
| Risque | un même climate de circuit piloté par plusieurs VTherm de pièces → **commandes contradictoires** | un seul VTherm doit le piloter |
| Décision | **non créé** | **créé, en essai** (`iot-viessmann-essai-chauffage-v0.7.yaml`) |

Limites communes d'un climate de circuit :

- la cible est une consigne de circuit, pas de pièce ;
- un seul point de consigne : la période réduite reste gérée par la
  Vitotronic avec `0x3307` ;
- pas de preset tant que Éco/Party ne sont pas validés ;
- OFF = « ECS seule » (`00`), jamais arrêt de la chaudière ;
- la température de départ n'apparaît pas dans le climate (entité séparée).

**Recommandation pour cette installation mixte :**

- A1 : approche A seule. Les VTherm de pièces existants restent sur leurs
  TRV, et la consigne A1 reste fixe ou n'est ajustée que lentement (§ 6.2).
- M2 : climate VissLink, piloté par **un seul** VTherm `over_climate` dédié,
  sans auto-régulation au départ (§ 5).

## 5. VTherm dédié au plancher (M2), pas à pas

> **Avertissement : ne pas associer le climate M2 à un VTherm qui alternerait
> fréquemment HEAT/OFF.** Chaque passage HEAT/OFF du climate est une
> écriture du programme M2 `0x3301` (`03` / `00`) dans la mémoire de la
> régulation. Il arrête puis relance le chauffage d'un plancher dont
> l'inertie se compte en heures. Désactiver dans ce VTherm tout ce qui
> peut éteindre le sous-jacent : démarrage/arrêt automatique (*auto-start/
> stop*), détection d'ouverture, présence et mouvement, délestage, et toute
> automatisation qui bascule `hvac_mode`. VissLink n'envoie rien si le
> programme lu correspond déjà, mais ne limite pas une alternance lente et
> répétée. Le plus sûr au départ : **laisser les modes non autorisés**
> (`enable_mode_writes` / `enable_m2_mode_write` à `false`), et ne laisser
> passer que la consigne.
>
> **L'intégration Versatile Thermostat n'est pas validée par la v0.7.** Elle
> sera validée séparément, après l'essai matériel des consignes (§ 8). Ce
> guide décrit la configuration prévue.

Prérequis :

- écriture de `0x3306` validée sur la chaudière (procédure
  `consignes-chauffage-v0.7.md` § 5) ;
- `0x0898` validée comme ambiance Vitotrol. Observée cohérente avec la
  température intérieure affichée par la Vitotrol, pas encore validée
  (codage, comportement sans Vitotrol) ;
- les modes ne seront commandés que si l'écriture de `0x3301` est autorisée
  (`enable_mode_writes`, `enable_m2_mode_write`).

1. **Flasher** l'essai v0.7 avec `enable_writes: true` et
   `enable_heating_setpoint_writes: true` (et rien d'autre au début).
2. Dans Home Assistant, vérifier le climate « Plancher chauffant » :
   - la cible est la consigne confort de M2 ;
   - la température actuelle est l'ambiance Vitotrol ;
   - le mode et l'action correspondent au panneau.
3. *Paramètres > Appareils et services > Ajouter une intégration >
   Versatile Thermostat*, type **« Thermostat sur un climate »**
   (`over_climate`). Nom par exemple « Plancher - VTherm ».
4. **Sous-jacent** : uniquement le climate « Plancher chauffant »
   (`climate.<plancher_chauffant>`). **Ne jamais** l'ajouter à un autre
   VTherm.
5. **Sonde de température** : « Plancher chauffant - Température ambiante
   (Vitotrol) » (`sensor.<plancher_ambiance_vitotrol>`). Sonde extérieure :
   celle de la Vitotronic si elle est exposée, sinon une sonde existante.
6. **Modes** : chauffage seulement. La climatisation n'existe pas.
7. **Presets** : Confort et Éco cohérents avec les limites (16..22 °C, par
   exemple 20 et 19). Éviter les changements fréquents : chaque changement
   est une écriture dans la mémoire de la régulation. Au plus quelques
   écritures par jour ; la garde VissLink impose 30 s minimum.
8. **Auto-régulation** : **désactivée** (« Aucune »). La Vitotronic régule
   déjà le départ par sa loi d'eau. Un ajustement automatique de la cible
   par VTherm créerait une **boucle de régulation** sur un système dont le
   temps de réponse se compte en heures.
9. **Fonctions à laisser désactivées au départ** :
   - détection d'ouverture ;
   - présence et mouvement ;
   - délestage ;
   - « chaudière centrale » pour ce VTherm.
10. **Horaires** : un seul maître pour la bascule jour/nuit.
    - Recommandé : la **programmation M2 de la Vitotronic** gère les périodes
      confort et réduite. VTherm reste sur un preset fixe.
    - Si VTherm doit gérer les horaires : la Vitotronic continue de passer
      en réduite `0x3307` selon son propre programme. Il faut alors aligner
      les deux programmes, sinon une consigne VTherm de nuit agira sur la
      consigne **confort**, inutilisée la nuit.

**Plus tard, après des semaines d'observation** (départ `0x3900`, ambiance,
circulateur), une auto-régulation « légère » peut être essayée :

- un seul paramètre à la fois ;
- un intervalle long (≥ 60 min) ;
- un retour arrière documenté.

## 6. Demandes de chauffage des pièces (A1)

### 6.1 Option 1 : rien (recommandée au départ)

Les VTherm de pièces pilotent leurs TRV. La Vitotronic assure le départ par
sa loi d'eau. La consigne A1 reste celle du panneau.

### 6.2 Option 2 : ajustement lent de la consigne A1

Si toutes les pièces manquent durablement de chaleur, ou si toutes ont leurs
vannes presque fermées, ajuster la consigne confort A1 de ±1 °C, avec :

- au plus une modification toutes les 2 h ;
- des bornes (internes VissLink : 16..23 °C) ;
- un journal.

Exemple (identifiants à remplacer ; ne pas activer avant la validation de
`0x2306`) :

```yaml
template:
  - sensor:
      - name: "VTherm radiateurs - pièces en chauffe"
        unique_id: vtherm_radiateurs_en_chauffe
        state: >
          {{ [ 'climate.<vtherm_salon>', 'climate.<vtherm_chambre_1>',
               'climate.<vtherm_bureau>' ]
             | select('is_state_attr', 'hvac_action', 'heating')
             | list | count }}

automation:
  - alias: "Radiateurs A1 - relever la consigne de circuit (lent)"
    mode: single
    triggers:
      - trigger: numeric_state
        entity_id: sensor.vtherm_radiateurs_pieces_en_chauffe
        above: 2          # toutes les pièces en chauffe (3 sur 3)
        for: "01:30:00"
    conditions:
      - condition: template   # pas plus d'une fois toutes les 2 h
        value_template: >
          {{ now() - states.number['<radiateurs_consigne_confort>'].last_changed
             > timedelta(hours=2) }}
      - condition: numeric_state
        entity_id: number.<radiateurs_consigne_confort>
        below: 22
    actions:
      - action: number.set_value
        target:
          entity_id: number.<radiateurs_consigne_confort>
        data:
          value: "{{ states('number.<radiateurs_consigne_confort>') | int + 1 }}"
```

La symétrique (abaisser quand aucune pièce ne chauffe) suit le même
modèle. Le résultat de chaque écriture se lit dans « Radiateurs - Consigne
confort - État modification ». Un refus (autorisation, limites, trop tôt)
ne relance rien.

### 6.3 Option 3 : « chaudière centrale » de VTherm (déconseillée ici)

VTherm peut appeler un service d'activation et de désactivation selon un
nombre d'équipements actifs ou une puissance. Paramètres :

- délai d'activation ;
- délai de maintien (keep-alive) ;
- entités `binary_sensor.central_configuration_central_boiler`,
  `number.boiler_activation_threshold` et
  `sensor.nb_device_active_for_boiler` ;
- évènement `versatile_thermostat_central_boiler_event`.

Pour cette installation, elle reviendrait à basculer le programme A1
(`03` ↔ `00`) au gré des TRV. Ce serait :

- des écritures fréquentes dans la régulation ;
- un départ coupé puis relancé ;
- un conflit avec la loi d'eau ;
- et, si elle était branchée sur l'arrêt (`05`), plus de production d'ECS.

**À ne pas activer.** VissLink n'exposera jamais `05` par le climate. Si
elle est un jour étudiée :

- seulement sur A1 ;
- avec un délai d'activation et de désactivation de plusieurs dizaines de
  minutes ;
- jamais sur M2.

## 7. Risques de commandes contradictoires

| Risque | Prévention |
|---|---|
| Plusieurs VTherm pilotent le même climate de circuit | un seul VTherm M2 ; pas de climate A1 |
| VTherm + automatisation + panneau modifient la même consigne | un seul maître par consigne ; le panneau reste prioritaire (détecté et affiché, jamais corrigé automatiquement) |
| Horaires VTherm et Vitotronic décalés | un seul maître des horaires (§ 5, étape 10) |
| Auto-régulation VTherm sur le plancher | désactivée ; boucle lente d'heures |
| Rafale de commandes (automatisation en boucle) | garde VissLink : une modification à la fois, 15 s entre consignes, 30 s par consigne, refus journalisés, jamais mis en file |
| OFF interprété comme arrêt de la chaudière | OFF = `00` ECS seule ; `05` jamais commandé |
| VTherm qui alterne HEAT/OFF (auto-start/stop, ouverture, présence, délestage) | fonctions désactivées ; modes non autorisés au départ (§ 5, avertissement) |
| Commandes pendant la restauration du bouclage ECS | consignes différées jusqu'à la fin de la restauration |

## 8. Tests de validation

1. **Sans écriture** :
   - le climate suit le panneau et la Vitotrol (cible, mode, ambiance) ;
   - une commande depuis Home Assistant est refusée, et l'état reste celui
     lu.
2. **Consigne M2 seule** (`enable_heating_setpoint_writes`) :
   - `climate.set_temperature` +1 °C, puis retour ;
   - vérifier le panneau ;
   - suivre `0x3900` pendant 1 h.
3. **VTherm M2 sans auto-régulation** : 48 h. Vérifier :
   - nombre d'écritures (journal, état de la consigne) ;
   - absence de commande répétée ;
   - ambiance.
4. **Modes** (si validés) : HEAT/OFF depuis le climate, puis l'ECS est
   toujours produite en OFF. Compter les écritures de `0x3301` sur 48 h
   avec le VTherm : une alternance HEAT/OFF répétée est un échec.
5. **Coupure Optolink** (interface débranchée 5 min) :
   - aucune commande ;
   - le climate revient aux valeurs lues.
6. **Redémarrage de l'ESP** : aucune écriture au démarrage (journal sans
   `-> `).
7. **A1, option 2** (si retenue) : une semaine en journalisation seule
   (automatisation avec `notify` à la place de `number.set_value`) avant
   d'écrire.

Simulations correspondantes : `make cpp-test`, tests `sim_v07_*` et
`climate_*`.
