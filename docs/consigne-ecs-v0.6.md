# Consigne d'eau chaude sanitaire (v0.6, expérimental)

> **v1.0.0 :** document historique. Les options d'autorisation (`enable_*`,
> `allow_write`, `validated_days`), les entités « État modification / commande »
> et les exemples intermédiaires cités ici n'existent plus. État actuel :
> `docs/v1.0.0.md` et `examples/iot-viessmann-v1.0.0.yaml`.


> **Statut : NON validé sur la chaudière.** La lecture de `0x6300` est
> établie. L'écriture est documentée seulement pour une autre régulation
> (Vitotronic 200 KW2, vcontrold) et **n'a jamais été faite sur la
> Vitotronic 300 KW3**. Elle est désactivée par défaut et exige deux
> autorisations. Une lecture réussie ne prouve pas que le registre est
> inscriptible.

## 1. Ce qui existait déjà (v0.3)

`0x6300` est un `number` depuis la v0.3 (« ECS - Consigne », 40..60 °C dans
l'exemple de production), avec le mécanisme commun des consignes :

- état publié = toujours une valeur **lue**, jamais la valeur demandée ;
- liste blanche (10..60 °C, plage documentée par openv) ;
- une écriture à la fois, 10 s au moins entre deux, relecture de
  vérification immédiate, aucune réémission ;
- demande non émise en 120 s : abandonnée ;
- rien n'est écrit au démarrage.

Jusqu'à la v0.5.2, **`enable_writes: true` suffisait** pour écrire `0x6300`.
La v0.6 ferme ce chemin.

## 2. Analyse du registre `0x6300`

| Point | Constat | Source | Statut sur la KW3 |
|---|---|---|---|
| Fonction | consigne de température ECS (« WW-Solltemperatur ») | vcontrold `getTempWWsoll` / `setTempWWsoll`, wiki openv | lecture cohérente avec le panneau (v0.2) |
| Longueur, codage | 1 octet, entier non signé, °C, pas de 1 | vcontrold (unité `UT1U`), openv | lu en `uint8` depuis la v0.2 |
| Plage | 10..60 °C | openv | **à vérifier** : le maximum peut dépendre du codage de la régulation |
| Écriture | `01 F4 63 00 01 VV`, accusé `00` | vcontrold, Vitotronic 200 **KW2** | **jamais testée** |
| Effet d'un changement au panneau | visible en relecture ? délai ? | — | **à vérifier** (procédure § 6, étape 2) |
| Second point de consigne (anti-légionelles), hystérésis, limites de sécurité | autres paramètres de la régulation (codage) | notices Viessmann | **jamais lus ni écrits** par VissLink |

Une seule entité lit `0x6300` : le validateur d'adresses uniques refuse une
seconde déclaration (`sensor` et `number` sur la même adresse). Aucune
lecture concurrente n'est possible.

## 3. Protections (v0.6)

### Autorisations

```yaml
visslink:
  enable_writes: false              # général
  enable_dhw_setpoint_write: false  # consigne ECS (v0.6), exige enable_writes
```

| Autorisations | Consigne ECS | Consignes d'ambiance | Modes A1/M2 | Bouclage |
|---|---|---|---|---|
| aucune | refusée | refusées | refusés | refusé |
| `enable_writes` | **refusée** (nouveau) | écrites | refusés | refusé |
| `enable_writes` + `enable_dhw_setpoint_write` | écrite | écrites | refusés | refusé |
| `enable_writes` + mode/M2/programmations | **refusée** | écrites | selon leurs autorisations | selon la sienne |

Les autorisations sont vérifiées **à la demande** et **juste avant
l'émission**. `enable_dhw_setpoint_write: true` sans `enable_writes: true`
est refusé à la validation YAML ; l'activation est signalée à la
compilation (`DHW SETPOINT WRITE ENABLED`).

### Listes blanches et limites

- `DHW_SETPOINT_WRITE_WHITELIST = {0x6300}` (`write_guard.h`), distincte de
  celles des programmes et des blocs horaires. Aucune autre adresse.
- **Limites internes d'écriture : 50..60 °C** (`DHW_WRITE_MIN`,
  `DHW_WRITE_MAX`). Aucune configuration ne peut les élargir.
  `min_value`/`max_value` peuvent être plus larges pour l'**affichage**
  (exemples existants : 40..60, toujours valides). Ils ne peuvent que
  **restreindre** l'écriture (ex. 52..58).
- Pourquoi 50..60 : en dessous de 50 °C, stockage tiède prolongé (hygiène) ;
  au-dessus de 60 °C, sortie de la plage standard de la régulation et
  risque de brûlure. **À confirmer avec l'installateur** pour ce ballon
  Vitocell 300-V et ce réseau sanitaire (bouclage, mitigeurs).
- VissLink ne touche à aucun autre paramètre : second point de consigne
  anti-légionelles, hystérésis, limites de sécurité de la chaudière.

## 4. Cycle d'une modification

```text
Home Assistant (55) ─► autorisations ─► valeur (entier, bornes, 50..60) ─► liaison ─► opération en cours ?
                                                                                       │ refus : motif affiché, rien n'est émis
                                                                                       ▼
                        RELECTURE de 0x6300 (prioritaire, jamais entre une écriture et sa vérification)
                            = 55 ─► « Inchangée », rien n'est écrit
                            ≠ 55 ─► écriture 01 F4 63 00 01 37 (arbitre) ─► accusé ─► RELECTURE immédiate
                                       = 55 ─► « Confirmée » (publiée)
                                       ≠ 55 ─► « Échec : relu X » (X publié), pas de nouvel essai
                                       illisible ─► « Indéterminé », dernière valeur confirmée conservée
```

- La relecture **avant** l'écriture règle deux cas. La demande égale à la
  valeur réelle n'écrit rien. Une modification faite au panneau depuis la
  dernière lecture périodique est prise en compte. Exemple : panneau à 53,
  affichage encore à 50, demande 50 → 53 relu, 50 écrit. En v0.3, ce cas
  était jugé « inchangé ».
- L'accusé ne prouve rien : seule la relecture décide. Un accusé négatif
  suivi d'une relecture conforme est « confirmé » ; le journal garde
  l'accusé.

### Demandes répétées, redémarrage, liaison

| Situation | Comportement |
|---|---|
| Nouvelle demande pendant une opération | refusée (« modification déjà en cours »), la demande en cours n'est jamais remplacée |
| Nouvelle demande moins de 30 s après une écriture | refusée (« trop tôt ») |
| Relecture préalable non obtenue en 60 s ou illisible | abandon, rien n'est écrit |
| Écriture non émise dans les 120 s (liaison, file) | abandon, jamais émise plus tard |
| Liaison perdue au moment d'émettre | trame non émise ; relecture impossible : « Indéterminé », aucune réémission |
| Redémarrage de l'ESP | rien n'est persistant : aucune demande ne subsiste, rien n'est écrit |
| Modification au panneau, sans demande | publiée à la lecture périodique ; état « Modifiée hors de Home Assistant (panneau ?) : 50 -> 53 °C » ; jamais corrigée |

### Coexistence avec le bouclage ECS

- Une seule écriture à la fois (arbitre commun).
- **La restauration du bouclage passe avant.** Pendant une restauration ou une
  récupération, l'écriture de consigne ECS reste en attente dans l'arbitre
  (`WriteArbiter::next(…, deferred)`), même si elle est plus ancienne. Elle
  reste soumise à l'abandon après 120 s.
- La relecture préalable n'est jamais servie pendant les lectures du
  bouclage (horloge, bloc, vérification).
- Limite : une écriture de consigne **déjà démarrée** (en attente du `0x05`)
  à l'instant où la durée du bouclage expire est menée à son terme. La
  restauration attend alors une écriture et sa relecture, 5 à 9 s.
- La machine à états du bouclage (v0.5.2) n'est pas modifiée : la colle lit
  seulement son état (`boost_restore_priority`).

## 5. Entités et journaux

```yaml
number:
  - platform: visslink
    visslink_id: viessmann
    name: "ECS - Température de consigne"
    address: 0x6300
    min_value: 50
    max_value: 60
    step: 1
    update_interval: 60s          # 5 min en production
    unit_of_measurement: "°C"
    mode: box
    status:                        # facultatif, 0x6300 seulement
      name: "ECS - État modification consigne"
```

États affichés (exemples) :

- `Refusé : écriture de la consigne ECS désactivée (enable_dhw_setpoint_write: false)`
- `Refusé : valeur 45.0 °C hors limites (50-60 °C)`
- `Relecture de la consigne actuelle avant d'écrire 55 °C`
- `Écriture de 55 °C (actuelle 50 °C)`
- `Confirmée : 55 °C relue (précédente 50 °C, accusé reçu)`
- `Échec : relu 50 °C au lieu de 55 °C (accusé négatif) ; pas de nouvel essai`
- `Indéterminé : relecture impossible après l'écriture de 55 °C (trame non émise) ; dernière valeur confirmée 50 °C`

Journaux (niveau INFO, sauf trames en DEBUG) :

- demande avec valeur précédente ;
- autorisations vérifiées ;
- valeur relue avant écriture ;
- trame `0x6300 -> 37 (write)` ;
- accusé ;
- relecture et résultat de la comparaison ;
- chronologie de l'écriture ;
- motif précis de tout refus (WARN).

## 6. Procédure d'essai sur la chaudière

Configuration :
[`iot-viessmann-essai-consigne-ecs-v0.6.yaml`](../examples/iot-viessmann-essai-consigne-ecs-v0.6.yaml).
C'est la production, avec la consigne renommée, bornée à 50..60, lue toutes
les 60 s et son état. Toutes les autorisations sont à `false`.

1. **Lire** : flasher l'essai tel quel. Relever « ECS - Température de
   consigne » et la comparer au panneau. Relever la trame brute
   (`0x6300 <- 32` = 50 °C). Noter la valeur initiale **sur papier**.
2. **Suivi du panneau** : modifier la consigne ECS sur le panneau (+1 °C),
   attendre au plus 60 s. L'entité doit suivre et l'état afficher
   « Modifiée hors de Home Assistant ». Noter le délai. Rétablir la valeur
   sur le panneau.
3. **Refus** : demander une valeur depuis Home Assistant. Attendu : « Refusé :
   écritures désactivées », aucune trame `->` dans le journal.
4. **Autorisations** (après accord explicite) : mettre à la main
   `enable_writes: true` et `enable_dhw_setpoint_write: true`, **rien
   d'autre**. Compiler : vérifier `DHW SETPOINT WRITE ENABLED` et l'absence
   de `MODE`, `M2` et `SCHEDULE … ENABLED`. Flasher.
5. **Modification minimale** : valeur initiale + 1 °C, dans 50..60, hors
   d'une charge ECS en cours. Suivre le journal : relecture préalable,
   `0x6300 -> XX (write)`, accusé, relecture, « Confirmée ».
6. **Panneau** : vérifier la nouvelle valeur sur l'afficheur de la Vitotronic.
7. **Relecture Optolink** : attendre une lecture périodique (60 s) ; la
   valeur doit rester celle écrite.
8. **Restaurer** : demander la valeur initiale ; « Confirmée » ; vérifier le
   panneau.
9. **Désactiver** : remettre les deux autorisations à `false`, reflasher,
   confirmer la valeur finale (panneau et Home Assistant), puis revenir à la
   production.

Au moindre écart (« Échec », « Indéterminé », panneau différent) : ne plus
rien demander, désactiver les écritures et rétablir la valeur **sur le
panneau** à partir du relevé papier.

## 7. Points restant à valider sur la KW3

1. Acceptation de l'écriture d'un octet en `0x6300`, et forme de l'accusé.
2. Plage réellement acceptée. La régulation borne-t-elle (55 relu pour 58
   demandé) ? Couvert par la relecture.
3. Délai de prise en compte d'une modification du panneau sur `0x6300`.
4. Effet sur la production ECS : déclenchement d'une charge quand la
   consigne monte, hystérésis.
5. Persistance après une coupure de la chaudière (mémoire de la régulation).
6. Compatibilité de 50..60 °C avec le ballon, le bouclage et la prévention
   des légionelles : **avis de l'installateur**.
7. Usure de la mémoire de la régulation : usage ponctuel (30 s minimum entre
   deux écritures, aucune automatisation recommandée).

## 8. Mesures

Simulation (tests `sim_v06_*`, `VL_REPORT=1`) :

| Resynchro | Demande → confirmation | Brûleur sur 6 h 30, 3 modifications/h | Écart max. brûleur (réf. sans demande) |
|---|---|---|---|
| 2 s | 5,0 s | 804/804 | 36,5 s (30,4 s) |
| 3 s | 8,0 s | 804/804 | 40,4 s (37,3 s) |
| 4 s | 9,0 s | 804/804 | 54,5 s (44,4 s) |

Compilation D1 mini (ESPHome 2026.10.2) :

| Configuration | RAM avant (v0.5.2) | RAM v0.6 | Flash avant | Flash v0.6 |
|---|---|---|---|---|
| Production (`iot-viessmann.yaml`) | 38 232 o | 38 548 o (+316, 47,1 %) | 473 445 o | 481 233 o (+7 788) |
| Essai consigne ECS | — | 38 644 o (47,2 %) | — | 481 329 o |
| Fixture v0.4.1 (`make compat`) | 35 696 o | 36 224 o (+528) | 451 441 o | 460 109 o |

La RAM ajoutée vient de l'état de la transaction (texte d'état, champs) et
des motifs de refus en anglais, que l'ESP8266 garde en RAM. Les messages en
français sont en flash (`PSTR`).
